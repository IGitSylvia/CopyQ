/*
    Copyright (c) 2020, Lukas Holecek <hluk@email.cz>

    This file is part of CopyQ.

    CopyQ is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    CopyQ is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with CopyQ.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "filewatcher.h"

#include "common/contenttype.h"
#include "common/log.h"
#include "item/serialize.h"

#include <QAbstractItemModel>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QMimeData>
#include <QRegularExpression>
#include <QUrl>

#include <array>
#include <vector>

const QLatin1String mimeExtensionMap(COPYQ_MIME_PREFIX_ITEMSYNC "mime-to-extension-map");
const QLatin1String mimeBaseName(COPYQ_MIME_PREFIX_ITEMSYNC "basename");
const QLatin1String mimeNoSave(COPYQ_MIME_PREFIX_ITEMSYNC "no-save");
const QLatin1String mimeSyncPath(COPYQ_MIME_PREFIX_ITEMSYNC "sync-path");
const QLatin1String mimeNoFormat(COPYQ_MIME_PREFIX_ITEMSYNC "no-format");
const QLatin1String mimeUnknownFormats(COPYQ_MIME_PREFIX_ITEMSYNC "unknown-formats");
const QLatin1String mimePrivatePrefix(COPYQ_MIME_PREFIX_ITEMSYNC_PRIVATE);
const QLatin1String mimeOldBaseName(COPYQ_MIME_PREFIX_ITEMSYNC_PRIVATE "old-basename");
const QLatin1String mimeHashPrefix(COPYQ_MIME_PREFIX_ITEMSYNC_PRIVATE "hash");

struct Ext {
    Ext() : extension(), format() {}

    Ext(const QString &extension, const QString &format)
        : extension(extension)
        , format(format)
    {}

    QString extension;
    QString format;
};

using Exts = std::vector<Ext>;

struct BaseNameExtensions {
    explicit BaseNameExtensions(const QString &baseName = QString(),
                                const Exts &exts = Exts())
        : baseName(baseName)
        , exts(exts)
    {}
    QString baseName;
    Exts exts;
};

namespace {

const QLatin1String dataFileSuffix("_copyq.dat");
const QLatin1String noteFileSuffix("_note.txt");

const int defaultUpdateFocusItemsIntervalMs = 10000;
const int batchItemUpdateIntervalMs = 100;

const qint64 sizeLimit = 10 << 20;

FileFormat getFormatSettingsFromFileName(const QString &fileName,
                                         const QList<FileFormat> &formatSettings,
                                         QString *foundExt = nullptr)
{
    for (const auto &format : formatSettings) {
        for ( const auto &ext : format.extensions ) {
            if ( fileName.endsWith(ext) ) {
                if (foundExt)
                    *foundExt = ext;
                return format;
            }
        }
    }

    return FileFormat();
}

void getBaseNameAndExtension(const QString &fileName, QString *baseName, QString *ext,
                             const QList<FileFormat> &formatSettings)
{
    ext->clear();

    const FileFormat fileFormat = getFormatSettingsFromFileName(fileName, formatSettings, ext);

    if ( !fileFormat.isValid() ) {
        const int i = fileName.lastIndexOf('.');
        if (i != -1)
            *ext = fileName.mid(i);
    }

    *baseName = fileName.left( fileName.size() - ext->size() );

    if ( baseName->endsWith('.') ) {
        baseName->chop(1);
        ext->prepend('.');
    }
}

const std::array<Ext, 12> &fileExtensionsAndFormats()
{
    static const std::array<Ext, 12> exts = {
        Ext(noteFileSuffix, mimeItemNotes),
        Ext(".txt", mimeText),
        Ext(".html", mimeHtml),
        Ext(".uri", mimeUriList),
        Ext(".png", "image/png"),
        Ext("_inkscape.svg", "image/x-inkscape-svg-compressed"),
        Ext(".svg", "image/svg+xml"),
        Ext(".bmp", "image/bmp"),
        Ext(".gif", "image/gif"),
        Ext(".jpg", "image/jpeg"),
        Ext(".xml", "application/xml"),
        Ext(".xml", "text/xml"),
    };

    return exts;
}

QString findByFormat(const QString &format, const QList<FileFormat> &formatSettings)
{
    // Find in default extensions.
    for (const auto &ext : fileExtensionsAndFormats()) {
        if (ext.format == format)
            return ext.extension;
    }

    // Find in user defined extensions.
    for (const auto &fileFormat : formatSettings) {
        if ( !fileFormat.extensions.isEmpty() && fileFormat.itemMime != "-"
             && format == fileFormat.itemMime )
        {
            return fileFormat.extensions.first();
        }
    }

    return QString();
}

Ext findByExtension(const QString &fileName, const QList<FileFormat> &formatSettings)
{
    // Is internal data format?
    if ( fileName.endsWith(dataFileSuffix) )
        return Ext(dataFileSuffix, mimeUnknownFormats);

    // Find in user defined formats.
    bool hasUserFormat = false;
    for (const auto &format : formatSettings) {
        for (const auto &ext : format.extensions) {
            if ( fileName.endsWith(ext) ) {
                if ( format.itemMime.isEmpty() )
                    hasUserFormat = true;
                else
                    return Ext(ext, format.itemMime);
            }
        }
    }

    // Find in default formats.
    for (const auto &ext : fileExtensionsAndFormats()) {
        if ( fileName.endsWith(ext.extension) )
            return ext;
    }

    return hasUserFormat ? Ext(QString(), mimeNoFormat) : Ext();
}

bool saveItemFile(const QString &filePath, const QByteArray &bytes,
                  QStringList *existingFiles, bool hashChanged = true)
{
    if ( !existingFiles->removeOne(filePath) || hashChanged ) {
        QFile f(filePath);
        if ( !f.open(QIODevice::WriteOnly) || f.write(bytes) == -1 ) {
            log( QStringLiteral("ItemSync: %1").arg(f.errorString()), LogError );
            return false;
        }
    }

    return true;
}

bool canUseFile(const QFileInfo &info)
{
    return !info.fileName().startsWith(QLatin1Char('.'));
}

bool getBaseNameExtension(const QString &filePath, const QList<FileFormat> &formatSettings,
                          QString *baseName, Ext *ext)
{
    QFileInfo info(filePath);
    if ( !canUseFile(info) )
        return false;

    *ext = findByExtension(filePath, formatSettings);
    if ( ext->format.isEmpty() || ext->format == QLatin1String("-") )
        return false;

    const QString fileName = info.fileName();
    *baseName = fileName.left( fileName.size() - ext->extension.size() );

    return true;
}

BaseNameExtensionsList listFiles(const QStringList &files,
                                 const QList<FileFormat> &formatSettings,
                                 const int maxItemCount)
{
    BaseNameExtensionsList fileList;
    QMap<QString, int> fileMap;

    for (const auto &filePath : files) {
        QString baseName;
        Ext ext;
        if ( getBaseNameExtension(filePath, formatSettings, &baseName, &ext) ) {
            int i = fileMap.value(baseName, -1);
            if (i == -1) {
                i = fileList.size();
                fileList.append( BaseNameExtensions(baseName, {ext}) );
                fileMap.insert(baseName, i);
                if (fileList.size() >= maxItemCount)
                    break;
            } else {
                fileList[i].exts.push_back(ext);
            }
        }
    }

    return fileList;
}

bool isBaseNameLessThan(const QString &lhsBaseName, const QString &rhsBaseName)
{
    const bool isLhsOwn = FileWatcher::isOwnBaseName(lhsBaseName);
    const bool isRhsOwn = FileWatcher::isOwnBaseName(rhsBaseName);
    if (isLhsOwn && isRhsOwn)
        return lhsBaseName > rhsBaseName;
    if (isLhsOwn)
        return true;
    if (isRhsOwn)
        return false;
    return lhsBaseName < rhsBaseName;
}

/// Sorts own files (copyq_*) first by creation date newer first and other
/// files by name alphabetically.
QFileInfoList sortedFilesInfos(const QDir &dir, const QDir::Filters &itemFileFilter)
{
    QFileInfoList infoList = dir.entryInfoList(itemFileFilter);
    std::sort(std::begin(infoList), std::end(infoList), [](const QFileInfo &lhs, const QFileInfo &rhs){
        const QString lhsBaseName = lhs.baseName();
        const QString rhsBaseName = rhs.baseName();
        return isBaseNameLessThan(lhsBaseName, rhsBaseName);
    });
    return infoList;
}

QStringList listFiles(const QDir &dir)
{
    QStringList files;

    const QDir::Filters itemFileFilter = QDir::Files | QDir::Readable | QDir::Writable;
    // Get files ordered by name to achieve same result on multiple platforms.
    // NOTE: Sorting by modification time can be slow.
    for ( const auto &info : sortedFilesInfos(dir, itemFileFilter) ) {
        if ( canUseFile(info) )
            files.append(info.absoluteFilePath());
    }

    return files;
}

/// Return true only if no file name in @a fileNames starts with @a baseName.
bool isUniqueBaseName(const QString &baseName, const QStringList &fileNames,
                      const QStringList &baseNames = QStringList())
{
    if ( baseNames.contains(baseName) )
        return false;

    for (const auto &fileName : fileNames) {
        if ( fileName.startsWith(baseName) )
            return false;
    }

    return true;
}

void moveFormatFiles(const QString &oldPath, const QString &newPath,
                     const QVariantMap &mimeToExtension)
{
    for (const auto &extValue : mimeToExtension) {
        const QString ext = extValue.toString();
        QFile::rename(oldPath + ext, newPath + ext);
    }
}

void copyFormatFiles(const QString &oldPath, const QString &newPath,
                     const QVariantMap &mimeToExtension)
{
    for (const auto &extValue : mimeToExtension) {
        const QString ext = extValue.toString();
        QFile::copy(oldPath + ext, newPath + ext);
    }
}

void removeFormatFiles(const QString &path, const QVariantMap &mimeToExtension)
{
    for (const auto &extValue : mimeToExtension)
        QFile::remove(path + extValue.toString());
}

bool renameToUnique(
        const QDir &dir, const QStringList &baseNames, QString *name,
        const QList<FileFormat> &formatSettings)
{
    if ( name->isEmpty() ) {
        const auto dateFormat = QStringLiteral("yyyyMMddhhmmsszzz");
        const auto dateTime = QDateTime::currentDateTimeUtc();
        const auto now = dateTime.toString(dateFormat);
        *name = QStringLiteral("copyq_%1").arg(now);
    } else {
        // Replace/remove unsafe characters.
        name->replace( QRegularExpression(QLatin1String(R"(/|\\|^\.)")),
                       QLatin1String("_") );
        name->remove( QRegularExpression(QLatin1String(R"(\n|\r)")) );
    }

    const QStringList fileNames = dir.entryList();

    if ( isUniqueBaseName(*name, fileNames, baseNames) )
        return true;

    QString ext;
    QString baseName;
    getBaseNameAndExtension(*name, &baseName, &ext, formatSettings);

    qulonglong i = 0;
    int fieldWidth = 0;

    QRegularExpression re(QLatin1String(R"(\d+$)"));
    const auto m = re.match(baseName);
    if (m.hasMatch()) {
        const QString num = m.captured();
        i = num.toULongLong();
        fieldWidth = num.size();
        baseName = baseName.mid( 0, baseName.size() - fieldWidth );
    } else {
        baseName.append('-');
    }

    for (int counter = 0; counter < 99999; ++counter) {
        *name = baseName + QStringLiteral("%1").arg(++i, fieldWidth, 10, QLatin1Char('0')) + ext;
        if ( isUniqueBaseName(*name, fileNames, baseNames) )
            return true;
    }

    log( QStringLiteral(
                "ItemSync: Failed to find unique base name with prefix: %1")
            .arg(baseName), LogError);
    return false;
}

} // namespace

QString FileWatcher::getBaseName(const QModelIndex &index)
{
    return getBaseName( index.data(contentType::data).toMap() );
}

QString FileWatcher::getBaseName(const QVariantMap &data)
{
    return data.value(mimeBaseName).toString();
}

bool FileWatcher::isOwnBaseName(const QString &baseName)
{
    return baseName.startsWith(QLatin1String("copyq_"));
}

void FileWatcher::removeFilesForRemovedIndex(const QString &tabPath, const QModelIndex &index)
{
    const QAbstractItemModel *model = index.model();
    if (!model)
        return;

    const QString baseName = FileWatcher::getBaseName(index);
    if ( baseName.isEmpty() )
        return;

    // Check if item is still present in list (drag'n'drop).
    bool remove = true;
    for (int i = 0; i < model->rowCount(); ++i) {
        const QModelIndex index2 = model->index(i, 0);
        if ( index2 != index && baseName == FileWatcher::getBaseName(index2) ) {
            remove = false;
            break;
        }
    }
    if (!remove)
        return;

    const QVariantMap itemData = index.data(contentType::data).toMap();
    const QVariantMap mimeToExtension = itemData.value(mimeExtensionMap).toMap();
    if ( mimeToExtension.isEmpty() )
        QFile::remove(tabPath + '/' + baseName);
    else
        removeFormatFiles(tabPath + '/' + baseName, mimeToExtension);
}

Hash FileWatcher::calculateHash(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha1);
}

FileWatcher::FileWatcher(
        const QString &path,
        const QStringList &paths,
        QAbstractItemModel *model,
        int maxItems,
        const QList<FileFormat> &formatSettings,
        QObject *parent)
    : QObject(parent)
    , m_model(model)
    , m_formatSettings(formatSettings)
    , m_path(path)
    , m_valid(true)
    , m_maxItems(maxItems)
{
    m_updateTimer.setSingleShot(true);

    bool ok;
    const int interval = qEnvironmentVariableIntValue("COPYQ_SYNC_UPDATE_INTERVAL_MS", &ok);
    m_interval = ok && interval > 0 ? interval : defaultUpdateFocusItemsIntervalMs;

    connect( &m_updateTimer, &QTimer::timeout,
             this, &FileWatcher::updateItems );

    connect( m_model, &QAbstractItemModel::rowsInserted,
             this, &FileWatcher::onRowsInserted );
    connect( m_model, &QAbstractItemModel::rowsAboutToBeRemoved,
             this, &FileWatcher::onRowsRemoved );
    connect( model, &QAbstractItemModel::rowsMoved,
             this, &FileWatcher::onRowsMoved );
    connect( m_model, &QAbstractItemModel::dataChanged,
             this, &FileWatcher::onDataChanged );

    if (model->rowCount() > 0)
        saveItems(0, model->rowCount() - 1);

    prependItemsFromFiles( QDir(path), listFiles(paths, m_formatSettings, m_maxItems) );
}

bool FileWatcher::lock()
{
    if ( !m_valid )
        return false;

    m_valid = false;
    return true;
}

void FileWatcher::unlock()
{
    m_valid = true;
}

QVariantMap FileWatcher::itemDataFromFiles(const QDir &dir, const BaseNameExtensions &baseNameWithExts)
{
    QVariantMap dataMap;
    QVariantMap mimeToExtension;

    updateDataAndWatchFile(dir, baseNameWithExts, &dataMap, &mimeToExtension);

    if ( !mimeToExtension.isEmpty() ) {
        const QString baseName = QFileInfo(baseNameWithExts.baseName).fileName();
        dataMap.insert(mimeBaseName, baseName);
        dataMap.insert(mimeOldBaseName, baseName);
        dataMap.insert(mimeExtensionMap, mimeToExtension);
    }

    return dataMap;
}

void FileWatcher::prependItemsFromFiles(const QDir &dir, const BaseNameExtensionsList &fileList)
{
    QVector<QVariantMap> items;
    items.reserve(fileList.size());

    for (auto it = fileList.rbegin(); it != fileList.rend(); ++it) {
        const QVariantMap item = itemDataFromFiles(dir, *it);
        if ( !item.isEmpty() )
            items.append(item);
    }

    createItems(items, 0);
}

void FileWatcher::insertItemsFromFiles(const QDir &dir, const BaseNameExtensionsList &fileList)
{
    if ( fileList.isEmpty() )
        return;

    QVector<QVariantMap> items;
    items.reserve(fileList.size());
    for (const auto &baseNameWithExts : fileList) {
        const QVariantMap item = itemDataFromFiles(dir, baseNameWithExts);
        // Skip if the associated file was just removed.
        if ( !item.isEmpty() )
            items.append(item);
    }

    int row = 0;
    int i = 0;
    for (; i < items.size(); ++i) {
        const QVariantMap &item = items[i];
        const QString &newBaseName = getBaseName(item);
        for ( ; row < m_model->rowCount(); ++row ) {
            const QString rowBaseName = getBaseName( m_model->index(row, 0) );
            if ( isBaseNameLessThan(newBaseName, rowBaseName) )
                break;
        }

        if ( row >= m_model->rowCount() )
            break;

        createItems({item}, row);
        ++row;
    }

    // Append rest of the items.
    if ( i < items.size() ) {
        const int space = m_maxItems - m_model->rowCount();
        if (space <= 0)
            return;

        items.erase(items.begin(), items.begin() + i);
        if ( space < items.size() )
            items.erase(items.begin(), items.begin() + space);
        createItems( items, m_model->rowCount() );
    }

    // TODO: Remove excesive items, but not pinned.
    // This must be done elsewhere using QTimer 0.
}

void FileWatcher::updateItems()
{
    if ( !lock() ) {
        m_updateTimer.start(m_interval);
        return;
    }

    QElapsedTimer t;
    t.start();

    m_lastUpdateTimeMs = QDateTime::currentMSecsSinceEpoch();

    const QDir dir(m_path);

    if ( m_batchIndexData.isEmpty() ) {
        const QStringList files = listFiles(dir);
        m_fileList = listFiles(files, m_formatSettings, m_maxItems);
        m_batchIndexData.reserve(m_model->rowCount());
        for (int row = 0; row < m_model->rowCount(); ++row) {
            const QModelIndex index = m_model->index(row, 0);
            if ( !oldBaseName(index).isEmpty() )
                m_batchIndexData.append(index);
        }

        m_lastBatchIndex = -1;
        if ( t.elapsed() > 100 )
            log( QStringLiteral("ItemSync: Files listed in %1 ms").arg(t.elapsed()) );
    }

    for ( int i = m_lastBatchIndex + 1; i < m_batchIndexData.size(); ++i ) {
        const QPersistentModelIndex &index = m_batchIndexData[i];
        if ( !index.isValid() )
            continue;
        const QString baseName = oldBaseName(index);
        if ( baseName.isEmpty() )
            continue;

        const auto it = std::find_if(
            std::begin(m_fileList), std::end(m_fileList),
            [&](const BaseNameExtensions &baseNameExtensions) {
                return baseNameExtensions.baseName == baseName;
            });

        QVariantMap dataMap;
        QVariantMap mimeToExtension;

        if ( it != m_fileList.cend() ) {
            updateDataAndWatchFile(dir, *it, &dataMap, &mimeToExtension);
            m_fileList.erase(it);
        }

        if ( mimeToExtension.isEmpty() ) {
            m_model->removeRow(index.row());
        } else {
            dataMap.insert(mimeBaseName, baseName);
            dataMap.insert(mimeExtensionMap, mimeToExtension);
            updateIndexData(index, &dataMap);
        }

        if ( t.elapsed() > 20 ) {
            COPYQ_LOG_VERBOSE( QStringLiteral("ItemSync: Items updated in %1 ms, last row %2/%3")
                 .arg(t.elapsed())
                 .arg(i + 1)
                 .arg(m_batchIndexData.size()) );
            m_lastBatchIndex = i;
            unlock();
            m_updateTimer.start(batchItemUpdateIntervalMs);
            return;
        }
    }

    t.restart();

    insertItemsFromFiles(dir, m_fileList);

    if ( t.elapsed() > 100 )
        log( QStringLiteral("ItemSync: Items created in %1 ms").arg(t.elapsed()) );

    m_fileList.clear();
    m_batchIndexData.clear();

    unlock();

    if (m_updatesEnabled)
        m_updateTimer.start(m_interval);
}

void FileWatcher::updateItemsIfNeeded()
{
    const auto time = QDateTime::currentMSecsSinceEpoch();
    if (time < m_lastUpdateTimeMs + m_interval)
        return;

    updateItems();
}

void FileWatcher::setUpdatesEnabled(bool enabled)
{
    m_updatesEnabled = enabled;
    if (enabled)
        updateItems();
    else if ( m_batchIndexData.isEmpty() )
        m_updateTimer.stop();
}

void FileWatcher::onRowsInserted(const QModelIndex &, int first, int last)
{
    saveItems(first, last);
}

void FileWatcher::onDataChanged(const QModelIndex &a, const QModelIndex &b)
{
    saveItems(a.row(), b.row());
}

void FileWatcher::onRowsRemoved(const QModelIndex &, int first, int last)
{
    const bool wasFull = m_maxItems >= m_model->rowCount();

    for ( const auto &index : indexList(first, last) ) {
        if ( !index.isValid() )
            continue;

        const QString baseName = oldBaseName(index);
        if ( isOwnBaseName(baseName) )
            removeFilesForRemovedIndex(m_path, index);
    }

    // If the tab is no longer full, try to add new files.
    if (wasFull)
        m_updateTimer.start(0);
}

void FileWatcher::onRowsMoved(const QModelIndex &, int start, int end, const QModelIndex &, int destinationRow)
{
    /* If own items were moved, change their base names in data to trigger
     * updating/renaming file names so they are also moved in other app instances.
     *
     * The implementation works in common cases but will fail if:
     * - Items are moved after the last own item.
     * - Items move repeatedly to some not-top position.
     */
    const int count = end - start + 1;

    const int baseRow = destinationRow < start
        ? destinationRow + count
        : destinationRow;
    QString baseName;
    if (destinationRow > 0) {
        const QModelIndex baseIndex = m_model->index(baseRow, 0);
        baseName = FileWatcher::getBaseName(baseIndex);

        if ( !isOwnBaseName(baseName) )
            return;

        if ( !baseName.isEmpty() && !baseName.contains(QLatin1Char('-')) )
            baseName.append(QLatin1String("-0000"));
    }

    for (int row = baseRow - 1; row >= baseRow - count; --row) {
        const auto index = m_model->index(row, 0);
        const QString currentBaseName = FileWatcher::getBaseName(index);
        if ( currentBaseName.isEmpty() || isOwnBaseName(currentBaseName) ) {
            const QVariantMap data = {{mimeBaseName, baseName}};
            m_model->setData(index, data, contentType::updateData);
        }
    }
}

QString FileWatcher::oldBaseName(const QModelIndex &index) const
{
    return index.data(contentType::data).toMap().value(mimeOldBaseName).toString();
}

void FileWatcher::createItems(const QVector<QVariantMap> &items, int targetRow)
{
    if ( items.isEmpty() )
        return;

    const int row = qMax( 0, qMin(targetRow, m_model->rowCount()) );
    if ( !m_model->insertRows(row, items.size()) )
        return;

    // Find index if it was moved after inserted.
    const int rows = m_model->rowCount();
    auto it = std::begin(items);
    for ( int i = 0; i < rows; ++i ) {
        const int row2 = (row + i) % rows;
        auto index = m_model->index(row2, 0);
        if ( getBaseName(index).isEmpty() ) {
            QVariantMap data = *it;
            updateIndexData(index, &data);
            ++it;
            if (it == std::end(items))
                break;
        }
    }
}

void FileWatcher::updateIndexData(const QModelIndex &index, QVariantMap *itemData)
{
    const QString baseName = getBaseName(*itemData);
    if ( baseName.isEmpty() ) {
        m_model->setData(index, *itemData, contentType::data);
        return;
    }

    itemData->insert(mimeOldBaseName, baseName);

    const QVariantMap mimeToExtension = itemData->value(mimeExtensionMap).toMap();
    for ( auto it = mimeToExtension.begin(); it != mimeToExtension.end(); ++it ) {
        if ( !it.key().startsWith(COPYQ_MIME_PREFIX_ITEMSYNC) ) {
            const QString ext = it.value().toString();
            const Hash hash = calculateHash(itemData->value(it.key()).toByteArray());
            const QString mime = mimeHashPrefix + ext;
            itemData->insert(mime, hash);
        }
    }
    m_model->setData(index, *itemData, contentType::data);
}

QList<QPersistentModelIndex> FileWatcher::indexList(int first, int last)
{
    QList<QPersistentModelIndex> indexList;
    indexList.reserve(last - first + 1);
    for (int i = first; i <= last; ++i)
        indexList.append( m_model->index(i, 0) );
    return indexList;
}

void FileWatcher::saveItems(int first, int last)
{
    if ( !lock() )
        return;

    if ( !m_batchIndexData.isEmpty() ) {
        COPYQ_LOG_VERBOSE( QStringLiteral("ItemSync: Batch updates interrupted") );
        m_batchIndexData.clear();
    }

    const auto indexList = this->indexList(first, last);

    // Create path if doesn't exist.
    QDir dir(m_path);
    if ( !dir.mkpath(".") ) {
        log( tr("Failed to create synchronization directory \"%1\"!").arg(m_path) );
        return;
    }

    if ( !renameMoveCopy(dir, indexList) )
        return;

    QStringList existingFiles = listFiles(dir);

    for (const auto &index : indexList) {
        if ( !index.isValid() )
            continue;

        const QString baseName = getBaseName(index);
        const QString filePath = dir.absoluteFilePath(baseName);
        QVariantMap itemData = index.data(contentType::data).toMap();
        QVariantMap oldMimeToExtension = itemData.value(mimeExtensionMap).toMap();
        QVariantMap mimeToExtension;
        QVariantMap dataMapUnknown;

        const QVariantMap noSaveData = itemData.value(mimeNoSave).toMap();

        QMutableMapIterator<QString, QVariant> it(itemData);
        while (it.hasNext()) {
            const auto item = it.next();
            const QString &format = item.key();
            if ( format.startsWith(COPYQ_MIME_PREFIX_ITEMSYNC) )
                continue; // skip internal data

            const QByteArray bytes = it.value().toByteArray();
            const Hash hash = calculateHash(bytes);

            if ( noSaveData.contains(format) && noSaveData[format].toByteArray() == hash ) {
                it.remove();
                continue;
            }

            bool hasFile = oldMimeToExtension.contains(format);
            const QString ext = hasFile ? oldMimeToExtension[format].toString()
                                        : findByFormat(format, m_formatSettings);

            if ( !hasFile && ext.isEmpty() ) {
                dataMapUnknown.insert(format, bytes);
            } else {
                mimeToExtension.insert(format, ext);
                const Hash oldHash = index.data(contentType::data).toMap().value(mimeHashPrefix + ext).toByteArray();
                if ( !saveItemFile(filePath + ext, bytes, &existingFiles, hash != oldHash) )
                    return;
            }
        }

        for ( auto it = oldMimeToExtension.constBegin();
              it != oldMimeToExtension.constEnd(); ++it )
        {
            if ( it.key().startsWith(mimeNoFormat) )
                mimeToExtension.insert( it.key(), it.value() );
        }

        if ( mimeToExtension.isEmpty() || !dataMapUnknown.isEmpty() ) {
            mimeToExtension.insert(mimeUnknownFormats, dataFileSuffix);
            QByteArray data = serializeData(dataMapUnknown);
            if ( !saveItemFile(filePath + dataFileSuffix, data, &existingFiles) )
                return;
        }

        if ( !noSaveData.isEmpty() || mimeToExtension != oldMimeToExtension ) {
            itemData.remove(mimeNoSave);

            for ( auto format = mimeToExtension.keyBegin(); format != mimeToExtension.keyEnd(); ++format )
                oldMimeToExtension.remove(*format);

            itemData.insert(mimeExtensionMap, mimeToExtension);
            updateIndexData(index, &itemData);

            // Remove files of removed formats.
            removeFormatFiles(filePath, oldMimeToExtension);
        }
    }

    unlock();
}

bool FileWatcher::renameMoveCopy(const QDir &dir, const QList<QPersistentModelIndex> &indexList)
{
    QStringList baseNames;

    for (const auto &index : indexList) {
        if ( !index.isValid() )
            continue;

        const QString olderBaseName = oldBaseName(index);
        const QString oldBaseName = getBaseName(index);
        QString baseName = oldBaseName;

        bool newItem = olderBaseName.isEmpty();
        bool itemRenamed = olderBaseName != baseName;
        if ( newItem || itemRenamed ) {
            if ( !renameToUnique(dir, baseNames, &baseName, m_formatSettings) )
                return false;
            itemRenamed = olderBaseName != baseName;
            baseNames.append(baseName);
        }

        QVariantMap itemData = index.data(contentType::data).toMap();
        const QString syncPath = itemData.value(mimeSyncPath).toString();
        bool copyFilesFromOtherTab = !syncPath.isEmpty() && syncPath != m_path;

        if (copyFilesFromOtherTab || itemRenamed) {
            const QVariantMap mimeToExtension = itemData.value(mimeExtensionMap).toMap();
            const QString newBasePath = m_path + '/' + baseName;

            if ( !syncPath.isEmpty() ) {
                copyFormatFiles(syncPath + '/' + oldBaseName, newBasePath, mimeToExtension);
            } else {
                // Move files.
                if ( !olderBaseName.isEmpty() )
                    moveFormatFiles(m_path + '/' + olderBaseName, newBasePath, mimeToExtension);
            }

            itemData.remove(mimeSyncPath);
            itemData.insert(mimeBaseName, baseName);
            updateIndexData(index, &itemData);

            if ( oldBaseName.isEmpty() && itemData.contains(mimeUriList) ) {
                if ( copyFilesFromUriList(itemData[mimeUriList].toByteArray(), index.row(), baseNames) )
                     m_model->removeRow(index.row());
            }
        }
    }

    return true;
}

void FileWatcher::updateDataAndWatchFile(const QDir &dir, const BaseNameExtensions &baseNameWithExts,
                            QVariantMap *dataMap, QVariantMap *mimeToExtension)
{
    const QString basePath = dir.absoluteFilePath(baseNameWithExts.baseName);

    for (const auto &ext : baseNameWithExts.exts) {
        if ( ext.format.isEmpty() )
            continue;

        const QString fileName = basePath + ext.extension;

        QFile f( dir.absoluteFilePath(fileName) );
        if ( !f.open(QIODevice::ReadOnly) )
            continue;

        if ( ext.extension == dataFileSuffix ) {
            QDataStream stream(&f);
            if ( deserializeData(&stream, dataMap) )
                mimeToExtension->insert(mimeUnknownFormats, dataFileSuffix);
        } else if ( f.size() > sizeLimit || ext.format.startsWith(mimeNoFormat)
                    || dataMap->contains(ext.format) )
        {
            mimeToExtension->insert(mimeNoFormat + ext.extension, ext.extension);
        } else {
            dataMap->insert(ext.format, f.readAll());
            mimeToExtension->insert(ext.format, ext.extension);
        }
    }
}

bool FileWatcher::copyFilesFromUriList(const QByteArray &uriData, int targetRow, const QStringList &baseNames)
{
    QMimeData tmpData;
    tmpData.setData(mimeUriList, uriData);

    const QDir dir(m_path);

    QVector<QVariantMap> items;

    for ( const auto &url : tmpData.urls() ) {
        if ( !url.isLocalFile() )
            continue;

        QFile f(url.toLocalFile());

        if ( !f.exists() )
            continue;

        QString extName;
        QString baseName;
        getBaseNameAndExtension( QFileInfo(f).fileName(), &baseName, &extName,
                                 m_formatSettings );

        if ( !renameToUnique(dir, baseNames, &baseName, m_formatSettings) )
            continue;

        const QString targetFilePath = dir.absoluteFilePath(baseName + extName);
        f.copy(targetFilePath);
        Ext ext;
        if ( getBaseNameExtension(targetFilePath, m_formatSettings, &baseName, &ext) ) {
            BaseNameExtensions baseNameExts(baseName, {ext});
            const QVariantMap item = itemDataFromFiles( QDir(m_path), baseNameExts );
            items.append(item);
            if (items.size() >= m_maxItems)
                break;
        }
    }

    createItems(items, targetRow);
    return !items.isEmpty();
}

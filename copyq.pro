TEMPLATE = subdirs

# generate cache file for build
cache()

SUBDIRS += src \
           plugins
TRANSLATIONS = translations/copyq_cs.ts \
               translations/copyq_de.ts \
               translations/copyq_es.ts

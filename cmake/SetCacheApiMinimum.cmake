# The cache extension changes INPUTSTREAM_TIMES, so an extended binary must
# require the same inputstream API in its generated add-on dependency.
file(READ "${ADP_MANIFEST}" ADP_MANIFEST_CONTENT)
string(REGEX REPLACE
       "(<import addon=\"kodi.binary.instance.inputstream\" minversion=\")[^\"]+(\" version=\")([^\"]+)(\")"
       "\\1\\3\\2\\3\\4" ADP_MANIFEST_CONTENT "${ADP_MANIFEST_CONTENT}")
file(WRITE "${ADP_MANIFEST}" "${ADP_MANIFEST_CONTENT}")

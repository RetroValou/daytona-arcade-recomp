# Writes OUT with the build's date (DAYTONA_VITA_BUILD_DATE, YYYY-MM-DD) for the
# menu's title line; run on every build, the file only changes with the date.
string(TIMESTAMP date "%Y-%m-%d")
file(WRITE "${OUT}.tmp" "#define DAYTONA_VITA_BUILD_DATE \"${date}\"\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${OUT}.tmp" "${OUT}")
file(REMOVE "${OUT}.tmp")

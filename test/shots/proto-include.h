// host shim: the real FreeType instead of Sony's proto-include.h (screenshot renderer only)
#pragma once
#include <stdint.h>
#include <string.h>
#include <ft2build.h>
#include FT_FREETYPE_H
typedef unsigned char FT_Byte;

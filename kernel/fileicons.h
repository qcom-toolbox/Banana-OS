#ifndef FILEICONS_H
#define FILEICONS_H

#include "types.h"

/*
 * Icons of the kinds of things Files and the Start menu show - folders,
 * kinds of files, disks, the libraries - drawn on a 16 x 16 grid at 16,
 * 32 or 48 pixels (Windows 7 style: a manila folder, a page with a folded
 * corner and a picture of what is in it).
 */
typedef enum {
    FI_FOLDER = 0, FI_FOLDER_OPEN,
    FI_FILE, FI_TEXT, FI_IMAGE, FI_AUDIO, FI_VIDEO, FI_PACKAGE, FI_ARCHIVE, FI_PROGRAM, FI_WEB, FI_PDF, FI_CODE,
    FI_COMPUTER, FI_DISK, FI_USB, FI_HOME, FI_DOCUMENTS, FI_PICTURES, FI_MUSIC, FI_VIDEOS, FI_DOWNLOADS,
    FI_FAVORITES, FI_LIBRARY, FI_SEARCH, FI_TRASH, FI_TRASH_FULL,
    FI_COUNT
} fileicon_t;

/* the kind of a file, from its name */
fileicon_t fileicon_for_name(const char* name);
/* "PNG image", "Text Document", "Banana OS app"... */
const char* fileicon_type_name(const char* name, char* buf, int cap);
/* draws it with its top-left at (x, y), size 16, 32 or 48 */
void fileicon_draw(fileicon_t kind, int x, int y, int size);

#endif

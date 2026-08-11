#pragma once

#include <cstring>

#define FOOTNOTE_NUMBER_LEN 32
// Bumped from 96 to 256 (upstream #2722): calibre-generated EPUBs with long
// filenames and URL-encoded characters routinely exceed 96 chars (e.g.
// "Author-Title_split_NNN.html#_ftnN" encoded is ~150 chars). See
// Section.cpp's SECTION_FILE_VERSION v40 -- this changes FootnoteEntry's
// serialized size in every cached Page.
#define FOOTNOTE_HREF_LEN 256

struct FootnoteEntry {
  char number[FOOTNOTE_NUMBER_LEN];
  char href[FOOTNOTE_HREF_LEN];

  FootnoteEntry() {
    number[0] = '\0';
    href[0] = '\0';
  }
};

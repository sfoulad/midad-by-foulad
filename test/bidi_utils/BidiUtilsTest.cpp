#include <gtest/gtest.h>

#include <BidiUtils.h>

namespace {

// Mirrors the paragraph-direction detection loop in ParsedText::layoutAndExtractLines:
// scan words in order, stop at the first one with a strong direction. This is the exact
// algorithm the ParsedText.cpp fix relies on -- validating it here pins the behavior
// without needing ParsedText's Epub/GfxRenderer dependencies.
bool detectParagraphIsRtl(const std::vector<std::string>& words, bool& isRtl) {
  for (const auto& word : words) {
    if (BidiUtils::firstStrongDirection(word.c_str(), isRtl, BidiUtils::RTL_PARAGRAPH_PROBE_DEPTH)) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST(FirstStrongDirection, ArabicWordIsRtl) {
  bool isRtl = false;
  EXPECT_TRUE(BidiUtils::firstStrongDirection("قال", isRtl));  // قال
  EXPECT_TRUE(isRtl);
}

TEST(FirstStrongDirection, LatinWordIsLtr) {
  bool isRtl = true;
  EXPECT_TRUE(BidiUtils::firstStrongDirection("Hello", isRtl));
  EXPECT_FALSE(isRtl);
}

TEST(FirstStrongDirection, AllDigitsFindsNothing) {
  bool isRtl = false;
  EXPECT_FALSE(BidiUtils::firstStrongDirection("12345", isRtl));
}

TEST(FirstStrongDirection, EmptyStringFindsNothing) {
  bool isRtl = false;
  EXPECT_FALSE(BidiUtils::firstStrongDirection("", isRtl));
}

TEST(FirstStrongDirection, LeadingArabicPunctuationSkippedToArabicLetter) {
  bool isRtl = false;
  // Arabic comma (U+060C) then قال -- comma is neutral, must skip through to the letter.
  EXPECT_TRUE(BidiUtils::firstStrongDirection("،قال", isRtl));
  EXPECT_TRUE(isRtl);
}

// Regression test for the bug this fix addresses: a paragraph opening with more than a
// few non-Arabic tokens (e.g. a numeric reference) before its first Arabic word was
// previously left undetected as RTL because the old probe only checked the first 3
// words. Digits/punctuation are weak/neutral and don't resolve a direction on their own,
// so the correct paragraph direction only reveals itself once the scan reaches the first
// real letter -- however far into the paragraph that is.
TEST(ParagraphDirectionDetection, ArabicAfterMoreThanThreeNeutralWordsIsDetectedAsRtl) {
  const std::vector<std::string> words = {"12", "-", "34", "قال"};  // ..., قال
  bool isRtl = false;
  ASSERT_TRUE(detectParagraphIsRtl(words, isRtl));
  EXPECT_TRUE(isRtl);
}

TEST(ParagraphDirectionDetection, ArabicAsFirstWordIsDetectedImmediately) {
  const std::vector<std::string> words = {"قال", "أحمد"};  // قال أحمد
  bool isRtl = false;
  ASSERT_TRUE(detectParagraphIsRtl(words, isRtl));
  EXPECT_TRUE(isRtl);
}

// A paragraph that is genuinely LTR at its first strong character (per UAX#9 P2/P3) must
// stay LTR even if it later contains an embedded Arabic word/quote -- this is what gates
// the whole detection block being entered in the first place is hasRtlWord in ParsedText,
// but the direction itself must still resolve from the first strong character, not from
// "any RTL content anywhere."
TEST(ParagraphDirectionDetection, LatinFirstWordStaysLtrDespiteLaterArabic) {
  const std::vector<std::string> words = {"Ahmad", "said", "قال"};  // ... قال
  bool isRtl = true;                                                              // start wrong on purpose
  ASSERT_TRUE(detectParagraphIsRtl(words, isRtl));
  EXPECT_FALSE(isRtl);
}

TEST(ParagraphDirectionDetection, AllNeutralWordsFindsNoDirection) {
  const std::vector<std::string> words = {"12", "-", "34", "..."};
  bool isRtl = false;
  EXPECT_FALSE(detectParagraphIsRtl(words, isRtl));
}

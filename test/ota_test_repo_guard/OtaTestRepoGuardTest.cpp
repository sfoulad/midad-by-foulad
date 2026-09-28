#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// CROSSPOINT_OTA_REPO exists so an isolated hardware OTA test can read a
// dedicated test repository instead of the production one. A release build that
// inherited it would look for firmware in the wrong place -- silently, since the
// only visible symptom is "no update available".
//
// These tests are the enforcement: they read platformio.ini and OtaUpdater.cpp
// from the source tree, so the invariant is checked by the already-required
// unit-tests job without touching any workflow file.
namespace {

std::string readFile(const std::string& relative) {
  std::ifstream in(std::string(REPO_ROOT_DIR) + "/" + relative);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// name -> body, for every [env:*] section in platformio.ini.
std::map<std::string, std::string> parseEnvSections(const std::string& ini) {
  std::map<std::string, std::string> envs;
  std::istringstream in(ini);
  std::string line, current;
  std::ostringstream body;
  const auto flush = [&]() {
    if (!current.empty()) envs[current] = body.str();
    body.str("");
    body.clear();
  };
  while (std::getline(in, line)) {
    if (!line.empty() && line[0] == '[') {
      flush();
      current = line.rfind("[env:", 0) == 0 ? line.substr(5, line.find(']') - 5) : "";
      continue;
    }
    // Strip comments before recording: a ';' or '#' line is documentation, not a
    // build flag, and a comment that merely *names* the macro must not read as an
    // override. (This test failed on exactly that before the strip was added.)
    const auto firstNonSpace = line.find_first_not_of(" \t");
    const bool isComment =
        firstNonSpace != std::string::npos && (line[firstNonSpace] == ';' || line[firstNonSpace] == '#');
    if (!current.empty() && !isComment) body << line << '\n';
  }
  flush();
  return envs;
}

}  // namespace

TEST(OtaTestRepoGuard, ProductionRepositoryIsTheCompiledInDefault) {
  const std::string src = readFile("src/network/OtaUpdater.cpp");
  ASSERT_FALSE(src.empty()) << "could not read OtaUpdater.cpp";
  // The #ifndef fallback must name the production repo, so any build that does
  // not explicitly override it reads the real channel.
  EXPECT_NE(src.find("#define CROSSPOINT_OTA_REPO \"sfoulad/midad-by-foulad\""), std::string::npos)
      << "the default update channel is no longer the production repository";
  EXPECT_NE(src.find("#ifndef CROSSPOINT_OTA_REPO"), std::string::npos) << "override is no longer guarded by #ifndef";
}

TEST(OtaTestRepoGuard, NoReleaseEnvironmentOverridesTheUpdateChannel) {
  const auto envs = parseEnvSections(readFile("platformio.ini"));
  ASSERT_FALSE(envs.empty()) << "could not parse platformio.ini";

  std::vector<std::string> offenders;
  for (const auto& [name, body] : envs) {
    const bool isRelease = name.find("gh_release") != std::string::npos;
    if (isRelease && body.find("CROSSPOINT_OTA_REPO") != std::string::npos) offenders.push_back(name);
  }
  EXPECT_TRUE(offenders.empty()) << "release environment(s) override the OTA repository: " << [&] {
    std::string s;
    for (const auto& o : offenders) s += o + " ";
    return s;
  }();
}

TEST(OtaTestRepoGuard, OnlyTheTestEnvironmentOverridesItAndItLabelsItself) {
  const auto envs = parseEnvSections(readFile("platformio.ini"));
  std::vector<std::string> overriding;
  for (const auto& [name, body] : envs) {
    if (body.find("CROSSPOINT_OTA_REPO") != std::string::npos) overriding.push_back(name);
  }
  ASSERT_EQ(overriding.size(), 1u) << "exactly one environment may override the OTA repository";
  EXPECT_EQ(overriding[0], "x4pro-otatest");

  // A build on a non-production channel must be identifiable from its version
  // string alone, so a hardware result cannot be mistaken for a release result.
  const std::string& body = envs.at("x4pro-otatest");
  EXPECT_NE(body.find("-otatest"), std::string::npos) << "test build does not mark CROSSPOINT_VERSION";
}

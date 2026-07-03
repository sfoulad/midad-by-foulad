#pragma once

// Non-secret catalog identity for the built-in "Foulad eBooks" home menu entry.
// Username/password are entered on-device on first use (FouladEbooksSetupActivity)
// and stored via OpdsServerStore — never hardcoded here, since this repo is public.
constexpr char FOULAD_EBOOKS_NAME[] = "Foulad eBooks";
constexpr char FOULAD_EBOOKS_URL[] = "https://foulad.one/opds";

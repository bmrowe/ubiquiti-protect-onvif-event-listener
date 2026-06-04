// Copyright 2026 Daniel W
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include <iostream>
#include <string>

#include "dump_sanitizer.hpp"

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const std::string& label) {
  if (cond) {
    ++g_pass;
  } else {
    ++g_fail;
    std::cerr << "FAIL: " << label << '\n';
  }
}

// ---------------------------------------------------------------
// IP remapping per the per-prefix trie spec.
// ---------------------------------------------------------------
static void test_ip_remap_basic() {
  onvif::DumpSanitizer s;
  // Spec from the original feature request:
  //   192.168.1.1   -> 1.1.1.1
  //   192.168.1.2   -> 1.1.1.2
  //   192.168.2.100 -> 1.1.2.1
  check(s.remap_ip("192.168.1.1")   == "1.1.1.1",   "192.168.1.1 -> 1.1.1.1");
  check(s.remap_ip("192.168.1.2")   == "1.1.1.2",   "192.168.1.2 -> 1.1.1.2");
  check(s.remap_ip("192.168.2.100") == "1.1.2.1",   "192.168.2.100 -> 1.1.2.1");
}

static void test_ip_remap_consistent() {
  onvif::DumpSanitizer s;
  // Same input must produce the same output across calls (within one
  // sanitiser instance).
  const std::string a1 = s.remap_ip("10.0.0.5");
  const std::string a2 = s.remap_ip("10.0.0.5");
  check(a1 == a2, "same IP remaps consistently");
  // A different IP at the same depth gets a different mapping.
  const std::string b = s.remap_ip("10.0.0.6");
  check(a1 != b, "different IPs do not collide");
}

static void test_ip_remap_independent_subtrees() {
  onvif::DumpSanitizer s;
  // First octet differs -> last-octet counter independent.
  s.remap_ip("10.0.0.1");      // 1.1.1.1
  s.remap_ip("10.0.0.2");      // 1.1.1.2
  const std::string r = s.remap_ip("172.0.0.1");  // new prefix 172
  check(r == "2.1.1.1", "fresh prefix resets all downstream counters");
}

static void test_ip_remap_invalid_unchanged() {
  onvif::DumpSanitizer s;
  // Out-of-range octets are not touched.
  check(s.remap_ip("999.0.0.0") == "999.0.0.0",
        "out-of-range octet returned unchanged");
  // Non-IP strings are not touched.
  check(s.remap_ip("not-an-ip") == "not-an-ip",
        "garbage input returned unchanged");
}

// ---------------------------------------------------------------
// IP substitution within text.
// ---------------------------------------------------------------
static void test_sanitize_ip_in_text() {
  onvif::DumpSanitizer s;
  const std::string in =
      "[192.168.1.108] received 1 event\n"
      "[192.168.1.108] alive: events_recv=42\n"
      "[192.168.1.109] received 1 event\n"
      "Watching camera 192.168.1.108\n";
  const std::string out = s.sanitize(in);
  // Same IP must map to same value across all lines.
  check(out.find("192.168.1.108") == std::string::npos,
        "no original IP leaked");
  check(out.find("192.168.1.109") == std::string::npos,
        "second IP also remapped");
  // First IP encountered is .108, mapped to 1.1.1.1; .109 -> 1.1.1.2.
  check(out.find("[1.1.1.1] received 1 event") != std::string::npos,
        "first IP -> 1.1.1.1");
  check(out.find("[1.1.1.2] received 1 event") != std::string::npos,
        "second IP -> 1.1.1.2");
  check(out.find("Watching camera 1.1.1.1") != std::string::npos,
        "IP in different context -> same mapping");
}

static void test_sanitize_ignores_versions() {
  onvif::DumpSanitizer s;
  // Three-octet version strings won't match the 4-octet pattern.
  const std::string out = s.sanitize("v1.4.6 deployed; protect 7.0.107");
  check(out.find("v1.4.6") != std::string::npos, "v1.4.6 untouched");
  check(out.find("7.0.107") != std::string::npos, "7.0.107 untouched");
}

// ---------------------------------------------------------------
// Credential redaction.
// ---------------------------------------------------------------
static void test_sanitize_wsse_tags() {
  onvif::DumpSanitizer s;
  const std::string in =
      "<wsse:Username>admin</wsse:Username>"
      "<wsse:Password Type=\"...PasswordDigest\">aGFzaA==</wsse:Password>"
      "<wsse:Nonce EncodingType=\"...Base64Binary\">QUJDREVG</wsse:Nonce>";
  const std::string out = s.sanitize(in);
  check(out.find("admin")      == std::string::npos, "username redacted");
  check(out.find("aGFzaA==")   == std::string::npos, "password digest redacted");
  check(out.find("QUJDREVG")   == std::string::npos, "nonce redacted");
  check(out.find("[REDACTED]") != std::string::npos,
        "redaction marker present");
}

static void test_sanitize_kv_passwords() {
  onvif::DumpSanitizer s;
  std::string out;
  out = s.sanitize("password=hunter2");
  check(out == "password=[REDACTED]", "password=hunter2 redacted");
  out = s.sanitize("Password = \"foo bar\"");
  check(out.find("foo bar") == std::string::npos,
        "quoted password value redacted");
  out = s.sanitize("passwd='x@y' followed");
  check(out.find("x@y") == std::string::npos,
        "single-quoted passwd redacted");
}

static void test_sanitize_url_creds() {
  onvif::DumpSanitizer s;
  const std::string in =
      "fetching http://admin:hunter2@192.168.1.108/snap";
  const std::string out = s.sanitize(in);
  check(out.find("admin:hunter2@") == std::string::npos,
        "URL creds redacted");
  check(out.find("192.168.1.108") == std::string::npos,
        "URL host IP also remapped");
  check(out.find("[REDACTED]:[REDACTED]@") != std::string::npos,
        "redacted creds form");
}

static void test_sanitize_basic_auth_header() {
  onvif::DumpSanitizer s;
  const std::string out = s.sanitize(
      "Authorization: Basic YWRtaW46aHVudGVyMg==");
  check(out.find("YWRtaW46aHVudGVyMg==") == std::string::npos,
        "basic auth token redacted");
}

static void test_sanitize_xuserid() {
  onvif::DumpSanitizer s;
  const std::string out = s.sanitize(
      "X-UserId: 65f1abc012def03e40000123\n");
  check(out.find("65f1abc012def03e40000123") == std::string::npos,
        "X-UserId redacted");
}

static void test_sanitize_user_kv() {
  onvif::DumpSanitizer s;
  // "user=postgres" in conn strings is sensitive enough to redact.
  const std::string out = s.sanitize(
      "host=/run/postgresql port=5433 dbname=unifi-protect user=postgres");
  check(out.find("user=[REDACTED]") != std::string::npos,
        "user= value redacted");
  // db name and port should not be touched.
  check(out.find("dbname=unifi-protect") != std::string::npos,
        "dbname preserved");
  check(out.find("port=5433") != std::string::npos,
        "port preserved");
}

// Test names are deliberately generic placeholders ("Lincoln", "Hayes",
// "Polk"), not values borrowed from any real deployment, so the source
// tree never grows a personally-identifying camera name even by mistake.
static void test_register_camera_name_basic() {
  onvif::DumpSanitizer s;
  const std::string label_a = s.register_camera_name("Lincoln");
  const std::string label_b = s.register_camera_name("Hayes");
  // Labels are the deterministic hash form, distinct per name.
  check(label_a.rfind("Camera-", 0) == 0,
        "label has Camera- prefix");
  check(label_a.size() == 7 + 8,  // "Camera-" + 8 hex chars
        "label is 15 chars total");
  check(label_a != label_b,
        "distinct names get distinct labels");
  // Idempotent: same name -> same label.
  check(s.register_camera_name("Lincoln") == label_a,
        "duplicate name reuses label");
  // Empty / whitespace names are no-ops, returned unchanged.
  check(s.register_camera_name("") == "",
        "empty name returned unchanged");
  check(s.register_camera_name("   ") == "   ",
        "whitespace name returned unchanged");
}

static void test_sanitize_camera_names() {
  onvif::DumpSanitizer s;
  const std::string label_a = s.register_camera_name("Lincoln");
  const std::string label_b = s.register_camera_name("Hayes");
  const std::string in =
      "{\"name\":\"Lincoln\",\"events_1h\":3}\n"
      "msg: motion at Hayes at 19:24\n"
      "another note about Lincoln\n";
  const std::string out = s.sanitize(in);
  check(out.find("Lincoln") == std::string::npos,
        "first name redacted everywhere");
  check(out.find("Hayes") == std::string::npos,
        "second name redacted");
  check(out.find(label_a) != std::string::npos &&
        out.find(label_b) != std::string::npos,
        "hashed labels present");
}

static void test_sanitize_camera_name_longest_first() {
  onvif::DumpSanitizer s;
  // Register the substring first.  Longest-first sort must still
  // protect the longer name from being chewed up.
  const std::string short_label = s.register_camera_name("Polk");
  const std::string long_label  = s.register_camera_name("Polk Doorbell");
  const std::string out = s.sanitize(
      "Polk Doorbell tripped; Polk motion later.\n");
  // The longer name must be replaced as a whole.
  check(out.find("Polk Doorbell") == std::string::npos,
        "longer name fully redacted");
  check(out.find("Polk motion") == std::string::npos,
        "shorter name redacted separately");
  check(out.find(long_label) != std::string::npos,
        "longer name maps to its hash label");
  check(out.find(short_label + " motion") != std::string::npos,
        "shorter standalone occurrence maps to its own hash label");
}

static void test_sanitize_camera_name_deterministic() {
  // Two independent sanitisers see the same name -> same label.
  // Guards against accidental introduction of run-local state in the
  // hashing path (e.g. a per-instance counter).
  onvif::DumpSanitizer a;
  onvif::DumpSanitizer b;
  check(a.register_camera_name("Tyler") == b.register_camera_name("Tyler"),
        "hash label is deterministic across instances");
}

int main() {
  test_ip_remap_basic();
  test_ip_remap_consistent();
  test_ip_remap_independent_subtrees();
  test_ip_remap_invalid_unchanged();
  test_sanitize_ip_in_text();
  test_sanitize_ignores_versions();
  test_sanitize_wsse_tags();
  test_sanitize_kv_passwords();
  test_sanitize_url_creds();
  test_sanitize_basic_auth_header();
  test_sanitize_xuserid();
  test_sanitize_user_kv();
  test_register_camera_name_basic();
  test_sanitize_camera_names();
  test_sanitize_camera_name_longest_first();
  test_sanitize_camera_name_deterministic();

  std::cout << "test_dump_sanitizer: " << g_pass << " passed, "
            << g_fail << " failed\n";
  return g_fail > 0 ? 1 : 0;
}

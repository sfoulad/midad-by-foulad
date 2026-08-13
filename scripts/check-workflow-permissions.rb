#!/usr/bin/env ruby
# check-workflow-permissions.rb
#
# Reads a single GitHub Actions workflow file's YAML content on stdin and
# checks its SEMANTIC permissions (workflow-level `permissions:` and every
# `jobs.<job>.permissions:` block) for statuses:write, checks:write, or the
# write-all shorthand. Exists because a plain text/regex scan for the
# literal bytes `statuses: write` is defeated by any equivalent-meaning
# YAML spelling -- `statuses: "write"`, extra spacing around the colon, a
# YAML alias resolving to the string "write", etc. -- all of which are
# ordinary valid YAML, not exotic bypasses. Parsing the document and
# reading the actual resolved values is the only way to catch the
# semantics rather than one exact spelling.
#
# Uses Ruby's bundled Psych (YAML.safe_load) rather than an installed
# package: Psych ships as part of Ruby's OWN standard library, not a
# separately versioned gem, so there is no `gem install`/network fetch
# step for a trusted, required security check to depend on -- and Ruby
# itself is part of GitHub's own ubuntu-latest runner image (no setup
# action needed). `safe_load` (not the unsafe `YAML.load`) restricts
# deserialization to plain data types (Hash/Array/String/Numeric/
# booleans/nil) -- no arbitrary Ruby object construction from the
# document, which matters here because the input is untrusted (a PR's own
# workflow file content, read via `git show`, never executed).
# `aliases: true` lets ordinary YAML anchors/aliases resolve normally
# (aliases are a standard YAML feature, not a code-execution risk the way
# custom type tags are) specifically so a value expressed via an alias
# still gets checked as its resolved string, not skipped.
#
# Second invariant, independent of the forbidden-permission scan above:
# any workflow that triggers on `pull_request_target` MUST declare an
# explicit top-level `permissions:` block. GitHub documents that
# `pull_request_target` runs with the BASE repository's token privileges
# unless the workflow explicitly narrows them -- a `pull_request_target`
# workflow with no `permissions:` key at all is relying entirely on the
# repository's default token permissions (this repo's own
# `default_workflow_permissions: read` is a real, separately-checked
# defense-in-depth fact, not something this parser can see or should
# assume; a future repo, or this one if that setting ever changes, could
# default to `write`). Requiring the declaration to merely EXIST (even
# `permissions: {}` or `permissions: read-all` count) forces every
# `pull_request_target` workflow to make its token scope an explicit,
# reviewable decision rather than an implicit one.
#
# `on:` detection handles every normal GitHub Actions trigger form --
# `on: pull_request_target`, `on: [push, pull_request_target]`, and the
# mapping form `on:\n  pull_request_target:\n    types: [...]` -- and
# Psych's YAML 1.1 quirk where an unquoted `on:` key parses as the
# *boolean* `true` rather than the string `"on"` (bareword `on`/`off`/
# `yes`/`no` are YAML 1.1 boolean literals): `workflow_triggers` checks
# both `doc["on"]` and `doc[true]`, whichever is actually present.
#
# Exit 0: no forbidden permission found, and (if triggered on
#         pull_request_target) an explicit permissions block exists.
# Exit 1: at least one violation found (printed to stdout, one per line)
#         -- a forbidden permission OR a missing explicit permissions
#         block on a pull_request_target workflow. This is a real,
#         successfully-parsed YAML document that fails the policy, not a
#         parse failure -- see exit 2 for that, a different case.
# Exit 2: the input could not be parsed as a YAML mapping at all -- the
#         caller MUST treat this as fail-closed (an error, not a pass),
#         never as "no forbidden permissions found."
require "yaml"

FORBIDDEN_KEYS = { "statuses" => "write", "checks" => "write" }.freeze
WRITE_ALL = "write-all"

def check_permissions(perms, source, violations)
  return if perms.nil?

  if perms.is_a?(String)
    violations << "#{source}: permissions: #{perms}" if perms == WRITE_ALL
  elsif perms.is_a?(Hash)
    perms.each do |key, value|
      key_s = key.to_s
      value_s = value.to_s
      next unless FORBIDDEN_KEYS[key_s] == value_s

      violations << "#{source}: #{key_s}: #{value_s}"
    end
  end
end

def workflow_triggers(doc)
  on_value = doc.key?("on") ? doc["on"] : doc[true]
  case on_value
  when String
    [on_value]
  when Array
    on_value.map(&:to_s)
  when Hash
    on_value.keys.map(&:to_s)
  else
    []
  end
end

content = $stdin.read

begin
  doc = YAML.safe_load(content, aliases: true)
rescue StandardError => e
  warn "check-workflow-permissions.rb: YAML parse error: #{e.message}"
  exit 2
end

unless doc.is_a?(Hash)
  warn "check-workflow-permissions.rb: top-level document is not a mapping"
  exit 2
end

violations = []

triggers = workflow_triggers(doc)
if triggers.include?("pull_request_target") && !doc.key?("permissions")
  violations << "pull_request_target workflow has no explicit top-level permissions declaration"
end

check_permissions(doc["permissions"], "workflow-level", violations)

jobs = doc["jobs"]
if jobs.is_a?(Hash)
  jobs.each do |job_name, job_def|
    next unless job_def.is_a?(Hash)

    check_permissions(job_def["permissions"], "jobs.#{job_name}", violations)
  end
end

if violations.empty?
  exit 0
else
  puts violations.join("\n")
  exit 1
end

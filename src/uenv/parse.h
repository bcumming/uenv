#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <util/expected.h>

#include <uenv/elastic.h>
#include <uenv/model.h>
#include <uenv/mount.h>
#include <uenv/settings.h>
#include <uenv/uenv.h>
#include <uenv/view.h>
#include <util/lex.h>
#include <util/parse.h>
#include <util/semver.h>

namespace uenv {

// the parsing scaffolding (parse_error, the PARSE macro, parse_string) lives in
// util so that it can be shared with other parsers (e.g. src/oci). uenv keeps
// the familiar unqualified name via this alias.
using parse_error = util::parse_error;

util::expected<std::vector<view_description>, parse_error>
parse_view_args(const std::string& arg);

util::expected<std::vector<env_view_description>, parse_error>
parse_env_view_description(const std::string& arg);

// the layer marker '+' at the start of a label (5-discovery.md, "Images and
// layers"), and the text after it
struct layer_marker {
    bool layer = false;
    std::string_view rest;
};
layer_marker parse_layer_marker(std::string_view in);

// one item of the list of uenv run/start and srun --uenv:
// [alias=][+](label|path)[:mount]
struct run_item {
    std::optional<std::string> alias;
    bool layer = false;
    uenv_description what;
};
util::expected<std::vector<run_item>, parse_error>
parse_run_items(const std::string& in);

// the form of a registry address (4-registry-ops.md, "Registries and
// addresses"), on the lexer. The parts are text: the caller makes the types.
struct address_text {
    enum class form { label, named, reference };
    form kind = form::label;
    // named: reg[/ns]::label
    std::string registry;
    std::vector<std::string> ns;
    // label and named: the text of the label (can be empty)
    std::string label;
    // reference: host/repository[:tag|@digest]
    std::string host;
    std::vector<std::string> repository;
    std::string reference;
    bool by_digest = false;
};
util::expected<address_text, std::string> parse_address_text(const std::string& in);

// the 'from' of a layer in a compose file: "[+]label" (a layer), or
// "label#alias" (the layer with that alias in an image), on the lexer
struct compose_from {
    std::string label;
    std::optional<std::string> alias;
};
util::expected<compose_from, std::string> parse_compose_from(const std::string& in);

// a name of a v1 label: [A-Za-z0-9_][A-Za-z0-9_.-]*, the whole input
util::expected<std::string, parse_error> parse_name(const std::string& in);

// a path that a user gives for a file or a directory (parse_path), made
// absolute against the current directory
util::expected<std::filesystem::path, std::string>
parse_file_path(std::string_view in);

// "alias:view": the stored form of a reference to a layer view
util::expected<layer_view_ref, std::string>
parse_layer_view_ref(const std::string& in);

// "key=value,...": the changes of copy --set. A key is version, release or
// meta.<key>. A value is a name; an empty value erases a meta key.
util::expected<std::vector<config_change>, std::string>
parse_config_changes(const std::string& in);

// "[item:]view,...": the views that a user names (--view, UENV_VIEW)
util::expected<std::vector<view_request>, std::string>
parse_view_requests(const std::string& in);

util::expected<std::vector<uenv_description>, parse_error>
parse_uenv_args(const std::string& arg);

util::expected<std::vector<mount_description>, parse_error>
parse_mount_list(const std::string& arg);

util::expected<uenv_date, parse_error> parse_uenv_date(const std::string& arg);

util::expected<std::string, parse_error> parse_path(const std::string& in);

util::expected<uenv_label, parse_error> parse_uenv_label(const std::string& in);

util::expected<uenv_description, parse_error>
parse_uenv_description(const std::string& in);

util::expected<uenv_nslabel, parse_error>
parse_uenv_nslabel(const std::string& in);

util::expected<uenv_registry_entry, parse_error>
parse_registry_entry(const std::string& in);

// TODO: remove as soon as the old config file format is dropped
util::expected<config_line, parse_error>
parse_config_line(const std::string& arg);

util::expected<std::optional<std::string>, parse_error>
parse_cluster_name(const std::string& in);

util::expected<std::string, parse_error>
parse_xthostname(const std::string& in);

util::expected<std::string, parse_error> parse_repo_name(const std::string& in);

util::expected<std::vector<repo_label>, parse_error>
parse_repo_list(const std::string& in);

util::expected<repo_label, parse_error> parse_repo_label(const std::string& in);

util::expected<util::semver, parse_error> parse_semver(const std::string& arg);

} // namespace uenv

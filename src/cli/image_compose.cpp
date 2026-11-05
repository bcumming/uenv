// `uenv image compose`: make an image from layers, with a compose file.

#include <string>

#include <argparse/argparse.h>
#include <fmt/core.h>
#include <nlohmann/json.hpp>

#include <uenv/ops.h>
#include <uenv/parse.h>
#include <uenv/session.h>
#include <util/fs.h>

#include "image.h"
#include "terminal.h"

namespace uenv {

using json = nlohmann::json;
using argparse::completion;

namespace {

struct compose_args {
    std::string file;
};

int image_compose(const compose_args& args, const global_settings& settings) {
    auto store = open_user_store(settings.config);
    if (!store) {
        term::error("{}", store.error());
        return 1;
    }
    auto file = parse_file_path(args.file);
    if (!file) {
        term::error("{}", file.error());
        return 1;
    }
    auto body = util::read_file(*file);
    if (!body) {
        term::error("{}", body.error());
        return 1;
    }
    auto spec = json::parse(*body, nullptr, false);
    if (spec.is_discarded()) {
        term::error("{} is not valid JSON", args.file);
        return 1;
    }
    auto record = compose(*store, spec, keys_of(settings.config));
    if (!record) {
        term::error("{}", record.error());
        return 1;
    }
    term::msg("composed {} id={} digest={}", label_string(record->cfg),
              record->id, record->digest.string());
    return 0;
}

} // namespace

argparse::command image_compose_command(const global_settings& settings) {
    argparse::command_builder<compose_args> c(
        "compose", "make an image from layers, with a compose file");
    c.add_positional("file", &compose_args::file, "the compose file (JSON)")
        .required()
        .complete(completion::file("*.json"));
    c.action([&settings](const compose_args& args) {
        return image_compose(args, settings);
    });
    return std::move(c).build();
}

} // namespace uenv

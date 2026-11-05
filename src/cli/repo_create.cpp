// `uenv repo create`: create a repository.

#include <optional>
#include <string>

#include <argparse/argparse.h>
#include <fmt/core.h>

#include <uenv/store.h>

#include "repo.h"
#include "repo_util.h"
#include "terminal.h"

namespace uenv {

using argparse::completion;

namespace {

struct repo_create_args {
    std::optional<std::string> path;
};

int repo_create(const repo_create_args& args, const global_settings& settings) {
    auto p = cli::repo_path(args.path, settings);
    if (!p) {
        term::error("{}", p.error());
        return 1;
    }
    auto s = store::create(*p);
    if (!s) {
        term::error("{}", s.error());
        return 1;
    }
    term::msg("the repository {} is ready", p->string());
    return 0;
}

} // namespace

argparse::command repo_create_command(const global_settings& settings) {
    argparse::command_builder<repo_create_args> c("create",
                                                  "create a repository");
    c.add_positional("path", &repo_create_args::path, "the repository")
        .complete(completion::directory());
    c.action([&settings](const repo_create_args& a) {
        return repo_create(a, settings);
    });
    return std::move(c).build();
}

} // namespace uenv

// The code that the `uenv image` commands share: the tables and JSON of
// records, and the registry context.
#pragma once

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <uenv/registry.h>
#include <uenv/store.h>

#include "uenv.h"

namespace uenv::cli {

// the time of a record: in a local repository the time of the add (not
// shown in the table), in a catalogue the time of the push (5-discovery.md,
// "Dates")
enum class record_time { added, pushed };

// the meta of a config as a JSON object
nlohmann::json meta_json(const std::map<uenv::name, std::string>& meta);

// print the records as JSON, or as a table with the columns uenv meta id
// size, and for a catalogue the date of the push
void print_records(const std::vector<object_record>& records, bool json_out,
                   bool no_header, record_time t);

// the context of the registry operations: the configuration, the cache and
// the credentials of each registry
registry::context make_context(const global_settings& settings);

} // namespace uenv::cli

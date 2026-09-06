#ifndef DEVILS_ENGINE_NETWORK_STATE_SCHEMA_H
#define DEVILS_ENGINE_NETWORK_STATE_SCHEMA_H

#include <devils_engine/utils/state_schema.h>

// Compatibility names for existing network callers. Canonical bytes and sectioned state documents
// are storage/transport-neutral and live in utils::serial.
namespace devils_engine::network {
using utils::serial::state_canonical_sink;
using utils::serial::state_compatibility_policy;
using utils::serial::state_load_result;
using utils::serial::state_load_status;
using utils::serial::state_reader;
using utils::serial::state_reader_like;
using utils::serial::state_schema;
using utils::serial::state_writer;
using utils::serial::state_writer_like;
using utils::serial::unique_state_section_ids_v;
} // namespace devils_engine::network

#endif

#ifndef TILE_FRONTIER_CORE_BROKER_H
#define TILE_FRONTIER_CORE_BROKER_H

#include <devils_engine/thread/mailbox.h>
#include <devils_engine/thread/spsc_queue.h>
#include <devils_engine/simul/standard_broker.h>

#include "messages.h"

namespace tile_frontier {
namespace core {

using namespace devils_engine;

// Единый holder всех межпоточных каналов (broker). Владелец — main: создаётся в runtime ДО подсистем
// и раздаётся указателем каждой (set_broker) до старта их потоков. ДВИЖКОВЫЕ каналы (окно/render/
// assets/sound) лежат плоско в simul::standard_broker и адресуются одинаково движком и проектом.
// Проект добавляет ТОЛЬКО свои каналы — без ручного reference-алиасинга. Топология проектных каналов:
//   mailbox<T>    — latest-wins (свежак вытесняет старое), drop-oldest штатно;
//   spsc_queue<T> — reliable/lossy FIFO (drop-newest при переполнении).
// Бюджеты ФИКСИРОВАНЫ конструктором (преаллокация; рантайм-роста пока нет).
struct broker : public simul::standard_broker {
  // main → render (latest-wins)
  thread::mailbox<command_draw_tiles>  draw_tiles;
  thread::mailbox<command_draw_actors> draw_actors;

  // main → assets
  thread::spsc_queue<command_load_chunk> load_chunk;   // reliable

  // assets → main
  thread::spsc_queue<command_chunk_loaded> chunk_loaded; // reliable (vector payload)

  broker()
    : simul::standard_broker()
    , load_chunk(256)
    , chunk_loaded(256)
  {}
};

}
}

#endif

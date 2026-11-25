#include <array>
#include <cstddef>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/network/document_chunks.h"
#include "devils_engine/network/endpoint.h"

namespace net = devils_engine::network;

TEST_CASE("parse_endpoint accepts dotted IPv4 with a port and refuses everything else [network::endpoint]") {
  const auto ok = net::parse_endpoint("127.0.0.1:41236");
  REQUIRE(ok.has_value());
  CHECK(ok->host == 0x7f000001u);
  CHECK(ok->port == 41236);
  CHECK(net::parse_endpoint("10.1.2.3:1")->host == 0x0a010203u);

  for (const auto* bad : {"127.0.0.1", "127.0.0.1:", "127.0.0.1:0", "127.0.0.1:65536", "127.0.0:80",
                          "127.0.0.1.5:80", "256.0.0.1:80", "a.b.c.d:80", "127..0.1:80", ":80",
                          "127.0.0.1:8x", "1234.0.0.1:80"}) {
    CAPTURE(bad);
    CHECK_FALSE(net::parse_endpoint(bad).has_value());
  }
  CHECK(net::parse_port("65535") == uint16_t(65535));
  CHECK_FALSE(net::parse_port("").has_value());
}

TEST_CASE("document chunks travel as offset + length and assemble strictly in order [network::document_chunks]") {
  const std::array<std::byte, 3> payload{std::byte{1}, std::byte{2}, std::byte{3}};
  std::vector<std::byte> wire;
  wire.reserve(32);
  {
    devils_engine::utils::serial::writer w(wire, false);
    w.u8(42); // тип сообщения потребителя
    CHECK_FALSE(net::write_document_chunk(w, 0, payload, 2)); // больше бюджета куска
  }
  wire.clear();
  {
    devils_engine::utils::serial::writer w(wire, false);
    w.u8(42);
    REQUIRE(net::write_document_chunk(w, 5, payload, 16));
  }
  CHECK(wire.size() == 1 + net::document_chunk_header_bytes + payload.size());
  devils_engine::utils::serial::reader r(wire);
  CHECK(r.u8() == 42);
  net::document_chunk_view view;
  REQUIRE(net::read_document_chunk(r, 16, view));
  CHECK(view.offset == 5);
  CHECK(view.payload.size() == 3);

  net::document_assembly document;
  CHECK_FALSE(document.begin(1u << 30, 1u << 20)); // предел до выделения памяти
  REQUIRE(document.begin(5, 1u << 20));
  CHECK_FALSE(document.append_in_order({1, payload})); // дыра
  CHECK(document.append_in_order({0, payload}));
  CHECK_FALSE(document.append_in_order({3, payload})); // за концом документа
  CHECK(document.append_in_order({3, std::span(payload).first(2)}));
  CHECK(document.complete());
  document.release();
  CHECK(document.received() == 5); // отчёт о передаче переживает освобождение
  CHECK_FALSE(document.complete());
}

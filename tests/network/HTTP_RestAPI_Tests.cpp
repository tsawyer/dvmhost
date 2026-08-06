// SPDX-License-Identifier: GPL-2.0-only
/*
 * Digital Voice Modem - Test Suite
 * GPLv2 Open Source. Use is subject to license terms.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 *  Copyright (C) 2026 Bryan Biedenkapp, N2PLL
 *
 */

#include <catch2/catch_test_macros.hpp>

#include "common/restapi/http/HTTPLexer.h"
#include "common/restapi/http/HTTPPayload.h"

#include <string>
#include <tuple>

using namespace restapi::http;

TEST_CASE("HTTPLexer parses a valid HTTP request", "[restapi][http][lexer]")
{
    const std::string request =
        "GET /api/v1/status HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    HTTPLexer lexer(false);
    HTTPPayload payload;

    auto result = lexer.parse(payload, request.begin(), request.end());

    REQUIRE(std::get<0>(result) == HTTPLexer::GOOD);
    REQUIRE(payload.method == "GET");
    REQUIRE(payload.uri == "/api/v1/status");
    REQUIRE(payload.httpVersionMajor == 1);
    REQUIRE(payload.httpVersionMinor == 1);
    REQUIRE(payload.headers.find("host") == "localhost");
    REQUIRE(payload.headers.find("Content-Length") == "0");
}

TEST_CASE("HTTPLexer reports incomplete request as indeterminate", "[restapi][http][lexer]")
{
    const std::string partialRequest =
        "GET /health HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 0\r\n"
        "\r";

    HTTPLexer lexer(false);
    HTTPPayload payload;

    auto result = lexer.parse(payload, partialRequest.begin(), partialRequest.end());

    REQUIRE(std::get<0>(result) == HTTPLexer::INDETERMINATE);
}

TEST_CASE("HTTPLexer rejects malformed request", "[restapi][http][lexer][negative]")
{
    const std::string malformedRequest =
        "GET /bad HTTP/1.x\r\n"
        "Host localhost\r\n"
        "\r\n";

    HTTPLexer lexer(false);
    HTTPPayload payload;

    auto result = lexer.parse(payload, malformedRequest.begin(), malformedRequest.end());

    REQUIRE(std::get<0>(result) == HTTPLexer::BAD);
}

TEST_CASE("HTTPLexer parses a valid HTTP response", "[restapi][http][lexer]")
{
    const std::string response =
        "HTTP/1.0 200 OK\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    HTTPLexer lexer(true);
    HTTPPayload payload;

    auto result = lexer.parse(payload, response.begin(), response.end());

    REQUIRE(std::get<0>(result) == HTTPLexer::GOOD);
    REQUIRE(payload.status == HTTPPayload::OK);
    REQUIRE(payload.httpVersionMajor == 1);
    REQUIRE(payload.httpVersionMinor == 0);
    REQUIRE(payload.headers.find("content-length") == "0");
}

TEST_CASE("HTTP status payload applies default REST headers", "[restapi][http][payload]")
{
    HTTPPayload payload = HTTPPayload::statusPayload(HTTPPayload::BAD_REQUEST, "application/json");

    REQUIRE(payload.isClientPayload == false);
    REQUIRE(payload.status == HTTPPayload::BAD_REQUEST);
    REQUIRE(payload.headers.find("Content-Type") == "application/json");
    REQUIRE(payload.headers.find("Content-Length") == std::to_string(payload.content.size()));
    REQUIRE(payload.headers.find("Server").empty() == false);
}

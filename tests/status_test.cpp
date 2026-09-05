// Unit tests for bedrockkv::Status.
//
// Testing philosophy for this project (design doc ch.7): the oracle style.
// For Status there is no external oracle, so we pin down the contract:
// default/factory -> code, message, ToString, equality, copy/move safety.
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "bedrockkv/status.h"

namespace {

using bedrockkv::Status;

TEST(StatusTest, DefaultConstructedIsOk) {
  const Status s;
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(s.code(), Status::Code::kOk);
  EXPECT_TRUE(s.message().empty());
  EXPECT_EQ(s.ToString(), "OK");
}

TEST(StatusTest, FactorySetsCodeAndMessage) {
  const Status nf = Status::NotFound("key missing");
  EXPECT_FALSE(nf.ok());
  EXPECT_EQ(nf.code(), Status::Code::kNotFound);
  EXPECT_EQ(nf.message(), "key missing");
  EXPECT_EQ(nf.ToString(), "NotFound: key missing");

  const Status c = Status::Corruption("crc mismatch");
  EXPECT_EQ(c.ToString(), "Corruption: crc mismatch");

  const Status io = Status::IOError("read failed");
  EXPECT_EQ(io.ToString(), "IOError: read failed");

  const Status ia = Status::InvalidArgument("empty key");
  EXPECT_EQ(ia.ToString(), "InvalidArgument: empty key");

  const Status ns = Status::NotSupported("scan");
  EXPECT_EQ(ns.ToString(), "NotSupported: scan");
}

TEST(StatusTest, EqualitySemantics) {
  EXPECT_EQ(Status::Ok(), Status());
  EXPECT_EQ(Status::NotFound("a"), Status::NotFound("a"));
  EXPECT_NE(Status::NotFound("a"), Status::NotFound("b"));
  EXPECT_NE(Status::NotFound("a"), Status::Corruption("a"));
}

TEST(StatusTest, SurvivesCopyAndMove) {
  const Status original = Status::IOError(std::string(4096, 'x'));
  const Status copied = original;            // copy
  const Status moved = std::move(copied);    // move out of a const copy

  EXPECT_EQ(original.ToString(), copied.ToString());
  EXPECT_EQ(original.ToString(), moved.ToString());
  EXPECT_EQ(original.code(), moved.code());
}

}  // namespace

#include <gtest/gtest.h>
#include "agentos/rpc.h"

using namespace agentos;

TEST(RPCTest, GenNewUUID) {
    std::string uuid1 = gen_new_uuid();
    std::string uuid2 = gen_new_uuid();
    EXPECT_NE(uuid1, uuid2);
    EXPECT_FALSE(uuid1.empty());
    EXPECT_FALSE(uuid2.empty());
}

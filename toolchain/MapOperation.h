#pragma once

#include "src/Params.h"
#include "test/network/GraphUtils.h"
#include "test/network/Utils.h"

void map_operation(const voyager::Operation& operation, const ScalarEnv& env,
                   std::deque<BaseParams*>& mapped_params);

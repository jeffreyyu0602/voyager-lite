#pragma once

#include "src/Params.h"
#include "runtime/GraphUtils.h"
#include "runtime/Utils.h"

void map_operation(const voyager::Operation& operation, const ScalarEnv& env,
                   std::deque<BaseParams*>& mapped_params);

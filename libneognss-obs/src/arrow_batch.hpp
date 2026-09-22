// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <nanoarrow/nanoarrow.h>
#include <stdexcept>
#include <string>

namespace neognss_obs {
struct ArrowBatchView {
    ArrowArrayView value{};
    ArrowBatchView(ArrowSchema *schema, ArrowArray *array) {
        ArrowError error{};
        if (ArrowArrayViewInitFromSchema(&value, schema, &error) ||
            ArrowArrayViewSetArray(&value, array, &error) ||
            ArrowArrayViewValidate(&value, NANOARROW_VALIDATION_LEVEL_FULL,
                                   &error)) {
            ArrowArrayViewReset(&value);
            throw std::runtime_error(
                std::string("Invalid CommonNEX Arrow batch: ") + error.message);
        }
    }
    ArrowBatchView(const ArrowBatchView &) = delete;
    ArrowBatchView &operator=(const ArrowBatchView &) = delete;
    ~ArrowBatchView() { ArrowArrayViewReset(&value); }
};
} // namespace neognss_obs

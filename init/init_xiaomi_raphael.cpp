/*
 * Copyright (C) 2022 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libinit_dalvik_heap.h>
#include <libinit_variant.h>

#include "vendor_init.h"

static const variant_info_t raphaelin_info = {
    .hwc_value = "INDIA",
    .sku_value = "",

    .brand = "Xiaomi",
    .device = "raphaelin",
    .marketname = "Redmi K20 Pro",
    .model = "MZB7751IN",
    .build_fingerprint = "Xiaomi/raphaelin/raphaelin:11/RKQ1.200826.002/V12.5.1.0.RFKINXM:user/release-keys",

    .nfc = false,
};

static const variant_info_t raphael_global_info = {
    .hwc_value = "GLOBAL",
    .sku_value = "",

    .brand = "Xiaomi",
    .device = "raphael",
    .marketname = "Mi 9T Pro",
    .model = "M1903F11G",
    .build_fingerprint = "Xiaomi/raphael/raphael:11/RKQ1.200826.002/V12.5.2.0.RFKMIXM:user/release-keys",

    .nfc = true,
};

static const variant_info_t raphael_info = {
    .hwc_value = "",
    .sku_value = "",

    .brand = "Xiaomi",
    .device = "raphael",
    .marketname = "Redmi K20 Pro",
    .model = "M1903F11A",
    .build_fingerprint = "Xiaomi/raphael/raphael:11/RKQ1.200826.002/V12.5.6.0.RFKCNXM:user/release-keys",

    .nfc = true,
};

static const std::vector<variant_info_t> variants = {
    raphaelin_info,
    raphael_global_info,
    raphael_info,
};

void vendor_load_properties() {
    search_variant(variants);
    set_dalvik_heap();
}

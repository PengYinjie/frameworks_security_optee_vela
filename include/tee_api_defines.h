/*
 * Copyright (c) 2016-2022, Linaro Limited
 * Copyright (C) 2020-2023 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef TEE_API_DEFINES_COMPAT_H
#define TEE_API_DEFINES_COMPAT_H

#include <nuttx/config.h>
#include_next <tee_api_defines.h>

#ifdef CONFIG_OPTEE_RPMB_FS
#define TEE_STORAGE_USER 0x80000100
#else
#define TEE_STORAGE_USER 0x80000000
#endif

#endif /* TEE_API_DEFINES_COMPAT_H */

/*
 * Copyright (c) 2016, Linaro Limited
 * Copyright (c) 2014, STMicroelectronics International N.V.
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

#include <assert.h>
#include <kernel/panic.h>

void __do_panic(const char* file, const int line, const char* func,
    const char* msg)
{
#if defined(CFG_TEE_CORE_DEBUG)
    EMSG("%s, %d, %s, %s\n", file, line, func, msg);
#else
    PANIC_WITH_REGS(msg, NULL);
    while (1)
        ;
#endif
}
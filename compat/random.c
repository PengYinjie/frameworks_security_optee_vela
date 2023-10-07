/*
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

#include <stdint.h>
#include <tee_api_types.h>
#include <rng_support.h>
#include <sys/random.h>

TEE_Result hw_get_random_bytes(void *buf, size_t blen)
{
	ssize_t ret;
	if (!buf)
		return TEE_ERROR_BAD_PARAMETERS;

	ret = getrandom(buf, blen, GRND_RANDOM);
	if (ret > 0)
		return TEE_SUCCESS;

	return TEE_ERROR_GENERIC;
}

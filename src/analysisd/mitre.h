/* Copyright (C) 2015-2019, Wazuh Inc.
 * Copyright (C) 2009 Trend Micro Inc.
 * All rights reserved.
 *
 * This program is a free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation.
 */

#ifndef MITRE_H
#define MITRE_H

#include "shared.h"
#include "os_net/os_net.h"
#include "headers/wazuhdb_op.h"

/**
 * @brief This function fills Hash Table using Mitre technique ID as Key and technique's tactics as info.
 * 
 * It connects to Wazuh-DB to get all IDs that are in mitre.db and their tactics and then it inserts these to Hash table.
 * 
 * @return int, it returns 0 on success and -1 on failure.
 */
int mitre_load(char *mode);

/**
 * @brief This function gets a Mitre technique ID's tactics from Hash Table 'mitre table'.
 * 
 * @param mitre_id Input parameter, Mitre technique ID (e.g. T1168). 
 * @return cJSON*, a cJSON array with tactic(s) or NULL if cJSON array is not found.   
 */
cJSON * mitre_get_attack(const char * mitre_id);

#endif /* MITRE_H */

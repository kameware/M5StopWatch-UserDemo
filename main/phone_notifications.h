/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct PhoneNotification {
    std::string app;
    std::string title;
    std::string body;
    std::string receivedAt;
    uint32_t sequence = 0;
};

inline constexpr const char* PHONE_NOTIFICATION_SERVICE_UUID = "7b3f0001-6d6f-4d35-9d65-534d53545700";
inline constexpr const char* PHONE_NOTIFICATION_WRITE_UUID   = "7b3f0002-6d6f-4d35-9d65-534d53545700";
inline constexpr const char* PHONE_NOTIFICATION_STATUS_UUID  = "7b3f0003-6d6f-4d35-9d65-534d53545700";

void phone_notifications_init();
bool phone_notifications_is_ble_ready();
bool phone_notifications_is_connected();
bool phone_notifications_is_ancs_ready();
uint32_t phone_notifications_count();
uint32_t phone_notifications_latest_sequence();
std::vector<PhoneNotification> phone_notifications_recent(std::size_t max_count);
void phone_notifications_clear();
void phone_notifications_receive_payload(const std::string& payload);

#pragma once

#include <string>

// Возвращает SHA-256 от input в виде 64 hex-символов (нижний регистр).
std::string sha256_hex(const std::string& input);

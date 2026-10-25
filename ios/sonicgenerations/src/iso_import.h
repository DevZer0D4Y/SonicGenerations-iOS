#pragma once
#include <filesystem>
#include <functional>
void SgImportISO(std::function<void(std::filesystem::path)> ready);

//
// Created by Yao ACHI on 08/11/2025.
//

#include <cstdint>
#include <iostream>
#include <string>
#include <typeinfo>
#include <vector>
#include <ylt/struct_pack.hpp>

#include "crc32c/crc32c.h"
#include "ylt/struct_pack/type_calculate.hpp"

struct PersonHeader
{
    std::int64_t id;
    std::string name;
    std::vector<char> data;
};

struct Person
{
    PersonHeader header;
    int age;
    double salary;
};

struct Entry
{
    std::uint64_t timestamp_ns;
    uint8_t flag = 0;
    std::string key;
    std::vector<char> value;

    size_t disk_size() const { return struct_pack::get_needed_size(*this); }
};

int main()
{
    const PersonHeader header{.id = 1, .name = "key\0with\nnull\tand\rspecial", .data = {1, 2, 3, 4}};
    const Person person1{.header = header, .age = 20, .salary = 1024.42};

    Entry entry{.timestamp_ns = 1234567890, .key = "hello", .value = {1, 2, 3, 4}};

    std::cout << struct_pack::get_needed_size(entry);

    // serialization in one line
    const std::vector<char> result = struct_pack::serialize(person1);
    auto sz = struct_pack::get_needed_size(person1);
    std::cout << result.size() << ":" << sz << std::endl;

    auto resp = struct_pack::deserialize<Person>(result);
    if (resp.value<>().header.data == std::vector<char>{1, 2, 3, 4})
    {
        std::cout << "ok" << std::endl;
    }

    std::cout << resp.value().header.name << std::endl;

    // Raw buffer
    const uint8_t buffer[] = {1, 2, 3, 4};
    uint32_t crc = crc32c::Crc32c(buffer, 4);
    std::cout << crc << std::endl;

    // std::string
    std::string data = "hello world";
    crc = crc32c::Crc32c(data);
    std::cout << crc << std::endl;

    // C++17 std::string_view
    std::string_view view = "test";
    crc = crc32c::Crc32c(view);
    std::cout << crc << std::endl;
}

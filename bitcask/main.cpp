//
// Created by Yao ACHI on 08/11/2025.
//

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include <typeinfo>
#include <ylt/struct_pack.hpp>
#include "crc32c/crc32c.h"

struct PersonHeader {
    std::int64_t id;
    std::string name;
};

struct Person {
    PersonHeader header;
    int age;
    double salary;
};


int main()
{
    PersonHeader header{.id = 1, .name = "hello struct pack"};
    Person person1{.header = header, .age = 20, .salary = 1024.42};

    // serialization in one line
    const std::vector<char> result = struct_pack::serialize(person1);
    std::cout << result.size() << std::endl;

    auto resp = struct_pack::deserialize<Person>(result);
    std::cout << typeid(resp.value<>()).name() << std::endl;

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

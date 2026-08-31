#include <iostream>
#include <vector>
#include <ranges>
#include <print>

static auto good() {
    std::vector<int> temp{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    return temp | std::views::filter([](int k) -> bool {
        return k % 2 == 0; 
    });
}

int main() {
    auto t = good();
    
    for (int x : t) { 
        std::cout << x << " ";
    }

    // Запускаем итерацию по t
    std::println("{}", t); 
}
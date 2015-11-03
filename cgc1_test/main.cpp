#include <cgc1/cgc1.hpp>
#include "bandit.hpp"
#include "../cgc1/src/allocator.hpp"
#ifdef _WIN32
#pragma optimize("", off)
#endif

using namespace bandit;
extern void gc_bandit_tests();
extern void bitmap_tests();
extern void slab_allocator_bandit_tests();
extern void allocator_block_tests();
extern void allocator_block_set_tests();
extern void allocator_tests();

go_bandit([]() {
  describe("Pages", []() {
    it("test1", []() {
      cgc1::slab_t page(cgc1::slab_t::page_size() * 50, cgc1::slab_t::find_hole(cgc1::slab_t::page_size() * 10000));
#ifndef __APPLE__
      AssertThat(page.expand(cgc1::slab_t::page_size() * 5000), IsTrue());
      AssertThat(page.size() >= cgc1::slab_t::page_size() * 5000, IsTrue());
#endif
      page.destroy();
    });
  });
  slab_allocator_bandit_tests();
  describe("Allocator", []() {
    allocator_block_tests();
    allocator_block_set_tests();
    allocator_tests();
    describe("thread_allocator", []() {
      void *memory1 = malloc(1000);
      void *memory2 = malloc(1000);
      it("find_block_set_id", []() {
        AssertThat(cgc1::details::allocator_t<>::this_thread_allocator_t::find_block_set_id(16), Equals(static_cast<size_t>(0)));
        AssertThat(cgc1::details::allocator_t<>::this_thread_allocator_t::find_block_set_id(17), Equals(static_cast<size_t>(1)));
        AssertThat(cgc1::details::allocator_t<>::this_thread_allocator_t::find_block_set_id(32), Equals(static_cast<size_t>(2)));
        AssertThat(cgc1::details::allocator_t<>::this_thread_allocator_t::find_block_set_id(524287),
                   Equals(static_cast<size_t>(15)));
        AssertThat(cgc1::details::allocator_t<>::this_thread_allocator_t::find_block_set_id(1000000),
                   Equals(static_cast<size_t>(16)));
        AssertThat(cgc1::details::allocator_t<>::this_thread_allocator_t::find_block_set_id(1000000000),
                   Equals(static_cast<size_t>(17)));
      });
      free(memory1);
      free(memory2);
    });
  });
  describe("Algo", []() {
    it("insert_replace #1", []() {
      std::array<int, 5> in_array{{0, 1, 2, 3, 4}};
      std::array<int, 5> out_array{{0, 2, 3, 4, 55}};
      cgc1::insert_replace(in_array.begin() + 1, in_array.begin() + 4, 55);
      AssertThat(in_array == out_array, IsTrue());
    });
    it("insert_replace #2", []() {
      std::array<int, 5> in_array{{0, 1, 2, 3, 4}};
      std::array<int, 5> out_array{{0, 2, 3, 3, 4}};
      cgc1::insert_replace(in_array.begin() + 1, in_array.begin() + 2, 3);
      AssertThat(in_array == out_array, IsTrue());
    });
    it("insert_replace #3", []() {
      std::array<int, 2> in_array{{0, 1}};
      std::array<int, 2> out_array{{1, 2}};
      cgc1::insert_replace(in_array.begin(), in_array.begin() + 1, 2);
      AssertThat(in_array == out_array, IsTrue());
    });
    it("insert_replace #3", []() {
      std::array<int, 2> in_array{{0, 1}};
      std::array<int, 2> out_array{{0, 5}};
      cgc1::insert_replace(in_array.begin() + 1, in_array.begin() + 1, 5);
      AssertThat(in_array == out_array, IsTrue());
    });
    it("insert_replace #3", []() {
      std::array<int, 2> in_array{{0, 5}};
      std::array<int, 2> out_array{{2, 5}};
      cgc1::insert_replace(in_array.begin(), in_array.begin(), 2);
      AssertThat(in_array == out_array, IsTrue());
    });
    it("rotate1", []() {
      ::std::array<int, 5> in_array{{0, 1, 2, 3, 4}};
      ::std::rotate(in_array.begin(), in_array.begin() + 2, in_array.end());
      ::std::array<int, 5> out_array{{2, 3, 4, 0, 1}};
      AssertThat(in_array == out_array, IsTrue());
    });
  });
  bitmap_tests();
  gc_bandit_tests();
});

int main(int argc, char *argv[])
{
  CGC1_INITIALIZE_THREAD();
  auto ret = bandit::run(argc, argv);
#ifdef _WIN32
  ::std::cin.get();
#endif
  cgc1::cgc_shutdown();
  return ret;
}


add_executable(unittest_seastore_journal
  test_seastore_journal.cc)
add_ceph_test(unittest_seastore_journal
  unittest_seastore_cache)
target_link_libraries(
  unittest_seastore_journal
  crimson::gtest
  crimson-seastore)

add_executable(unittest_seastore_cache
  test_seastore_cache.cc)
add_ceph_test(unittest_seastore_cache
  unittest_seastore_cache)
target_link_libraries(
  unittest_seastore_cache
  crimson::gtest
  crimson-seastore)

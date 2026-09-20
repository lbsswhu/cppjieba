#ifndef CPPJIEBA_DAT_BUILDER_HPP
#define CPPJIEBA_DAT_BUILDER_HPP

#include <memory>
#include <string>
#include <vector>
#include "DatModel.hpp"
#include "Trie.hpp"

namespace cppjieba {

enum class DatBuildStatus { Success, Unsupported, FieldLimit, ResourceLimit, ValidationFailed };

struct DatBuildOptions {
  size_t max_slots;
  size_t max_temporary_bytes;
  double max_build_ms;
  size_t max_unique_weights;
  DatBuildOptions()
      : max_slots(dat_detail::NO_CHECK),
        max_temporary_bytes(256U * 1024U * 1024U),
        max_build_ms(10000.0),
        max_unique_weights(dat_detail::NO_WEIGHT) {}
};

struct DatBuildStats {
  DatBuildStatus status;
  std::string reason;
  double build_ms;
  double layout_ms;
  double validation_ms;
  size_t slot_count;
  size_t logical_nodes;
  size_t unique_weights;
  size_t model_bytes;
  DatBuildStats()
      : status(DatBuildStatus::Unsupported), build_ms(0), layout_ms(0),
        validation_ms(0), slot_count(0), logical_nodes(0), unique_weights(0),
        model_bytes(0) {}
};

struct DatBuildResult {
  std::unique_ptr<const DatModel> model;
  DatBuildStats stats;
};

class DatBuilder {
 public:
  static DatBuildResult Build(const std::vector<const DictUnit*>& input,
                             double unknownWeight,
                             const DatBuildOptions& options = DatBuildOptions());
};

}  // namespace cppjieba
#endif

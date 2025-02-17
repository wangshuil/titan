#include "blob_gc_picker.h"

namespace rocksdb {
namespace titandb {

BasicBlobGCPicker::BasicBlobGCPicker(TitanDBOptions db_options,
                                     TitanCFOptions cf_options)
    : db_options_(db_options), cf_options_(cf_options) {}

BasicBlobGCPicker::~BasicBlobGCPicker() {}

std::unique_ptr<BlobGC> BasicBlobGCPicker::PickBlobGC(
    BlobStorage* blob_storage) {
  Status s;
  std::vector<BlobFileMeta*> blob_files;
  std::vector<BlobFileMeta*> cold_blob_files;

  uint64_t batch_size = 0;
  uint64_t estimate_output_size = 0;
  uint64_t cold_batch_size = 0;
  uint64_t cold_estimate_output_size = 0;
  //  ROCKS_LOG_INFO(db_options_.info_log, "blob file num:%lu gc score:%lu",
  //                 blob_storage->NumBlobFiles(),
  //                 blob_storage->gc_score().size());
  bool stop_picking = false;
  bool maybe_continue_next_time = false;
  uint64_t next_gc_size = 0;
  for (auto& gc_score : blob_storage->gc_score()) {
    auto blob_file = blob_storage->FindFile(gc_score.file_number).lock();
    if (!blob_file ||
        blob_file->file_state() == BlobFileMeta::FileState::kBeingGC) {
      // Skip this file id this file is being GCed
      // or this file had been GCed
      continue;
    }

    //    ROCKS_LOG_INFO(db_options_.info_log,
    //                   "file number:%lu score:%f being_gc:%d pending:%d, "
    //                   "size:%lu discard:%lu mark_for_gc:%d
    //                   mark_for_sample:%d", blob_file->file_number_,
    //                   gc_score.score, blob_file->being_gc,
    //                   blob_file->pending, blob_file->file_size_,
    //                   blob_file->discardable_size_,
    //                   blob_file->marked_for_gc_,
    //                   blob_file->marked_for_sample);

    if (!CheckBlobFile(blob_file.get())) {
      ROCKS_LOG_INFO(db_options_.info_log, "file number:%lu no need gc",
                     blob_file->file_number());
      continue;
    }

    if (!stop_picking) {
      if (blob_file.get()->is_cold_file()) {
        if (cold_estimate_output_size <
            cf_options_.merge_small_file_threshold) {
          cold_blob_files.push_back(blob_file.get());
        } else {
          blob_files.push_back(blob_file.get());
          batch_size += blob_file->file_size();
        }
        cold_estimate_output_size +=
            (blob_file->file_size() - blob_file->discardable_size());
        if (cold_estimate_output_size >=
            cf_options_.merge_small_file_threshold) /* TODO-- set args */
        {
          while (!cold_blob_files.empty()) {
            auto temp_blob_file = cold_blob_files[0];
            blob_files.push_back(temp_blob_file);
            batch_size += blob_file->file_size();
            cold_blob_files.erase(cold_blob_files.begin());
          }
        }
        if (cold_estimate_output_size >= cf_options_.blob_file_target_size ||
            batch_size >= cf_options_.max_gc_batch_size) /* TODO-- set args */
        {
          stop_picking = true;
        }
      } else {
        blob_files.push_back(blob_file.get());
        batch_size += blob_file->file_size();
        estimate_output_size +=
            (blob_file->file_size() - blob_file->discardable_size());
        if (batch_size >= cf_options_.max_gc_batch_size ||
            estimate_output_size >= cf_options_.blob_file_target_size) {
          // Stop pick file for this gc, but still check file for whether need
          // trigger gc after this
          stop_picking = true;
        }
      }
    } else {
      if (blob_file->file_size() <= cf_options_.merge_small_file_threshold ||
          blob_file->gc_mark() ||
          blob_file->GetDiscardableRatio() >=
              cf_options_.blob_file_discardable_ratio) {
        next_gc_size += blob_file->file_size();
        if (next_gc_size > cf_options_.min_gc_batch_size) {
          maybe_continue_next_time = true;
          ROCKS_LOG_INFO(db_options_.info_log,
                         "remain more than %" PRIu64
                         " bytes to be gc and trigger after this gc",
                         next_gc_size);
          break;
        }
      } else {
        break;
      }
    }
  }
  ROCKS_LOG_DEBUG(db_options_.info_log,
                  "got batch size %" PRIu64 ", estimate output %" PRIu64
                  " bytes",
                  batch_size, estimate_output_size);
  if (blob_files.empty() || batch_size < cf_options_.min_gc_batch_size)
    return nullptr;
  return std::unique_ptr<BlobGC>(new BlobGC(
      std::move(blob_files), std::move(cf_options_), maybe_continue_next_time));
}

bool BasicBlobGCPicker::CheckBlobFile(BlobFileMeta* blob_file) const {
  assert(blob_file->file_state() != BlobFileMeta::FileState::kInit);
  if (blob_file->file_state() != BlobFileMeta::FileState::kNormal) return false;

  return true;
}

}  // namespace titandb
}  // namespace rocksdb

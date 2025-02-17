#include "blob_gc_picker.h"

#include "blob_file_builder.h"
#include "blob_file_cache.h"
#include "blob_file_iterator.h"
#include "blob_file_reader.h"
#include "util/filename.h"
#include "util/testharness.h"

namespace rocksdb {
namespace titandb {

class BlobGCPickerTest : public testing::Test {
 public:
  std::unique_ptr<BlobStorage> blob_storage_;
  std::unique_ptr<BlobGCPicker> basic_blob_gc_picker_;

  BlobGCPickerTest() {}
  ~BlobGCPickerTest() {}

  void NewBlobStorageAndPicker(const TitanDBOptions& titan_db_options,
                               const TitanCFOptions& titan_cf_options) {
    auto blob_file_cache = std::make_shared<BlobFileCache>(
        titan_db_options, titan_cf_options, NewLRUCache(128), nullptr);
    blob_storage_.reset(new BlobStorage(titan_db_options, titan_cf_options, 0,
                                        blob_file_cache, nullptr));
    basic_blob_gc_picker_.reset(
        new BasicBlobGCPicker(titan_db_options, titan_cf_options));
  }

  void AddBlobFile(uint64_t file_number, uint64_t file_size,
                   uint64_t discardable_size, bool being_gc = false,
                   uint64_t is_cold_file = 0) {
    auto f =
        std::make_shared<BlobFileMeta>(file_number, file_size, is_cold_file);
    f->AddDiscardableSize(discardable_size);
    f->FileStateTransit(BlobFileMeta::FileEvent::kDbRestart);
    if (being_gc) {
      f->FileStateTransit(BlobFileMeta::FileEvent::kGCBegin);
    }
    blob_storage_->files_[file_number] = f;
  }

  void RemoveBlobFile(uint64_t file_number) {
    ASSERT_TRUE(blob_storage_->files_[file_number] != nullptr);
    blob_storage_->files_.erase(file_number);
  }

  void UpdateBlobStorage() { blob_storage_->ComputeGCScore(); }
};

TEST_F(BlobGCPickerTest, Basic) {
  TitanDBOptions titan_db_options;
  TitanCFOptions titan_cf_options;
  titan_cf_options.min_gc_batch_size = 0;
  NewBlobStorageAndPicker(titan_db_options, titan_cf_options);
  AddBlobFile(1U, 1U, 0U);
  UpdateBlobStorage();
  auto blob_gc = basic_blob_gc_picker_->PickBlobGC(blob_storage_.get());
  ASSERT_TRUE(blob_gc != nullptr);
  ASSERT_EQ(blob_gc->inputs().size(), 1);
  ASSERT_EQ(blob_gc->inputs()[0]->file_number(), 1U);
}
TEST_F(BlobGCPickerTest, Basic_cold) {
  TitanDBOptions titan_db_options;
  TitanCFOptions titan_cf_options;
  titan_cf_options.min_gc_batch_size = 0;
  NewBlobStorageAndPicker(titan_db_options, titan_cf_options);
  AddBlobFile(1U, 1U << 30, 1000U << 20, false, 1);
  UpdateBlobStorage();
  auto blob_gc = basic_blob_gc_picker_->PickBlobGC(blob_storage_.get());
  ASSERT_TRUE(blob_gc != nullptr);
  ASSERT_EQ(blob_gc->inputs().size(), 1);
  ASSERT_EQ(blob_gc->inputs()[0]->file_number(), 1U);
}

TEST_F(BlobGCPickerTest, BeingGC) {
  TitanDBOptions titan_db_options;
  TitanCFOptions titan_cf_options;
  titan_cf_options.min_gc_batch_size = 0;
  NewBlobStorageAndPicker(titan_db_options, titan_cf_options);
  AddBlobFile(1U, 1U, 0U, true);
  UpdateBlobStorage();
  auto blob_gc = basic_blob_gc_picker_->PickBlobGC(blob_storage_.get());
  ASSERT_EQ(nullptr, blob_gc);
  NewBlobStorageAndPicker(titan_db_options, titan_cf_options);
  AddBlobFile(1U, 1U, 0U, true);
  AddBlobFile(2U, 1U, 0U);
  UpdateBlobStorage();
  blob_gc = basic_blob_gc_picker_->PickBlobGC(blob_storage_.get());
  ASSERT_EQ(blob_gc->inputs().size(), 1);
  ASSERT_EQ(blob_gc->inputs()[0]->file_number(), 2U);
}

TEST_F(BlobGCPickerTest, BeingGC_cold) {
  TitanDBOptions titan_db_options;
  TitanCFOptions titan_cf_options;
  titan_cf_options.min_gc_batch_size = 0;
  NewBlobStorageAndPicker(titan_db_options, titan_cf_options);
  AddBlobFile(1U, 1U, 0U, true, 1);
  UpdateBlobStorage();
  auto blob_gc = basic_blob_gc_picker_->PickBlobGC(blob_storage_.get());
  ASSERT_EQ(nullptr, blob_gc);
  NewBlobStorageAndPicker(titan_db_options, titan_cf_options);
  AddBlobFile(1U, 1U, 0U, true, 1);
  AddBlobFile(2U, 1U << 30, 1000U << 20, false, 1);
  UpdateBlobStorage();
  blob_gc = basic_blob_gc_picker_->PickBlobGC(blob_storage_.get());
  ASSERT_EQ(blob_gc->inputs().size(), 1);
  ASSERT_EQ(blob_gc->inputs()[0]->file_number(), 2U);
}

TEST_F(BlobGCPickerTest, TriggerNext) {
  TitanDBOptions titan_db_options;
  TitanCFOptions titan_cf_options;
  titan_cf_options.max_gc_batch_size = 1 << 30;
  titan_cf_options.blob_file_target_size = 256 << 20;
  NewBlobStorageAndPicker(titan_db_options, titan_cf_options);
  AddBlobFile(1U, 1U << 30, 1000U << 20);  // valid_size = 24MB
  AddBlobFile(2U, 1U << 30, 512U << 20);   // valid_size = 512MB
  AddBlobFile(3U, 1U << 30, 512U << 20);   // valid_size = 512MB
  AddBlobFile(4U, 1U << 30, 512U << 20);   // valid_size = 512MB
  UpdateBlobStorage();
  auto blob_gc = basic_blob_gc_picker_->PickBlobGC(blob_storage_.get());
  ASSERT_TRUE(blob_gc != nullptr);
  ASSERT_EQ(blob_gc->trigger_next(), true);
  NewBlobStorageAndPicker(titan_db_options, titan_cf_options);
  AddBlobFile(1U, 1U << 30, 0U);  // valid_size = 1GB
  AddBlobFile(2U, 1U << 30, 0U);  // valid_size = 1GB
  AddBlobFile(3U, 1U << 30, 0U);  // valid_size = 1GB
  AddBlobFile(4U, 1U << 30, 0U);  // valid_size = 1GB
  UpdateBlobStorage();
  blob_gc = basic_blob_gc_picker_->PickBlobGC(blob_storage_.get());
  ASSERT_TRUE(blob_gc != nullptr);
  ASSERT_EQ(blob_gc->trigger_next(), false);
}

TEST_F(BlobGCPickerTest, TriggerNext_cold) {
  TitanDBOptions titan_db_options;
  TitanCFOptions titan_cf_options;
  titan_cf_options.max_gc_batch_size = 1 << 30;
  titan_cf_options.blob_file_target_size = 256 << 20;
  NewBlobStorageAndPicker(titan_db_options, titan_cf_options);
  AddBlobFile(1U, 1U << 30, 1000U << 20, false, 1);  // valid_size = 24MB
  AddBlobFile(2U, 1U << 30, 512U << 20, false, 1);   // valid_size = 512MB
  AddBlobFile(3U, 1U << 30, 512U << 20, false, 1);   // valid_size = 512MB
  AddBlobFile(4U, 1U << 30, 512U << 20, false, 1);   // valid_size = 512MB
  UpdateBlobStorage();
  auto blob_gc = basic_blob_gc_picker_->PickBlobGC(blob_storage_.get());
  ASSERT_TRUE(blob_gc != nullptr);
  ASSERT_EQ(blob_gc->trigger_next(), true);
  NewBlobStorageAndPicker(titan_db_options, titan_cf_options);
  AddBlobFile(1U, 1U << 30, 0U);  // valid_size = 1GB
  AddBlobFile(2U, 1U << 30, 0U);  // valid_size = 1GB
  AddBlobFile(3U, 1U << 30, 0U);  // valid_size = 1GB
  AddBlobFile(4U, 1U << 30, 0U);  // valid_size = 1GB
  UpdateBlobStorage();
  blob_gc = basic_blob_gc_picker_->PickBlobGC(blob_storage_.get());
  ASSERT_TRUE(blob_gc != nullptr);
  ASSERT_EQ(blob_gc->trigger_next(), false);
}

TEST_F(BlobGCPickerTest, PickFileAndTriggerNext) {
  TitanDBOptions titan_db_options;
  TitanCFOptions titan_cf_options;
  titan_cf_options.max_gc_batch_size = 1 << 30;
  titan_cf_options.blob_file_target_size = 256 << 20;
  NewBlobStorageAndPicker(titan_db_options, titan_cf_options);
  for (size_t i = 1; i < 41; i++) {
    // add 70 files with 10MB valid data each file
    AddBlobFile(i, titan_cf_options.blob_file_target_size, 246 << 20);
  }
  UpdateBlobStorage();
  int gc_times = 0;
  auto blob_gc = basic_blob_gc_picker_->PickBlobGC(blob_storage_.get());
  ASSERT_TRUE(blob_gc != nullptr);
  while (blob_gc != nullptr && blob_gc->trigger_next()) {
    gc_times++;
    ASSERT_EQ(blob_gc->trigger_next(), true);
    ASSERT_EQ(blob_gc->inputs().size(), 4);
    for (auto file : blob_gc->inputs()) {
      RemoveBlobFile(file->file_number());
    }
    UpdateBlobStorage();
    blob_gc = basic_blob_gc_picker_->PickBlobGC(blob_storage_.get());
  }
  ASSERT_EQ(gc_times, 9);
  ASSERT_TRUE(blob_gc != nullptr);
  ASSERT_EQ(blob_gc->inputs().size(), 4);
}

TEST_F(BlobGCPickerTest, PickFileAndTriggerNext_cold) {
  TitanDBOptions titan_db_options;
  TitanCFOptions titan_cf_options;
  titan_cf_options.max_gc_batch_size = 1 << 30;
  titan_cf_options.blob_file_target_size = 256 << 20;
  NewBlobStorageAndPicker(titan_db_options, titan_cf_options);
  for (size_t i = 1; i < 41; i++) {
    // add 70 files with 10MB valid data each file
    AddBlobFile(i, titan_cf_options.blob_file_target_size, 246 << 20, false, 1);
  }
  UpdateBlobStorage();
  int gc_times = 0;
  auto blob_gc = basic_blob_gc_picker_->PickBlobGC(blob_storage_.get());
  ASSERT_TRUE(blob_gc != nullptr);
  while (blob_gc != nullptr && blob_gc->trigger_next()) {
    gc_times++;
    ASSERT_EQ(blob_gc->trigger_next(), true);
    ASSERT_EQ(blob_gc->inputs().size(), 4);
    for (auto file : blob_gc->inputs()) {
      RemoveBlobFile(file->file_number());
    }
    UpdateBlobStorage();
    blob_gc = basic_blob_gc_picker_->PickBlobGC(blob_storage_.get());
  }
  ASSERT_EQ(gc_times, 9);
  ASSERT_TRUE(blob_gc != nullptr);
  ASSERT_EQ(blob_gc->inputs().size(), 4);
}

TEST_F(BlobGCPickerTest, ParallelPickGC) {
  TitanDBOptions titan_db_options;
  TitanCFOptions titan_cf_options;
  titan_cf_options.max_gc_batch_size = 1 << 30;
  titan_cf_options.blob_file_target_size = 256 << 20;
  NewBlobStorageAndPicker(titan_db_options, titan_cf_options);
  for (size_t i = 1; i < 9; i++) {
    // add 70 files with 10MB valid data each file
    AddBlobFile(i, titan_cf_options.blob_file_target_size, 246 << 20);
  }
  UpdateBlobStorage();
  auto blob_gc1 = basic_blob_gc_picker_->PickBlobGC(blob_storage_.get());
  ASSERT_TRUE(blob_gc1 != nullptr);
  ASSERT_EQ(blob_gc1->trigger_next(), true);
  ASSERT_EQ(blob_gc1->inputs().size(), 4);
  auto blob_gc2 = basic_blob_gc_picker_->PickBlobGC(blob_storage_.get());
  ASSERT_TRUE(blob_gc2 != nullptr);
  ASSERT_EQ(blob_gc2->trigger_next(), false);
  ASSERT_EQ(blob_gc2->inputs().size(), 4);
  for (auto file : blob_gc1->inputs()) {
    RemoveBlobFile(file->file_number());
  }
  for (auto file : blob_gc2->inputs()) {
    RemoveBlobFile(file->file_number());
  }
  UpdateBlobStorage();
}

TEST_F(BlobGCPickerTest, ParallelPickGC_cold) {
  TitanDBOptions titan_db_options;
  TitanCFOptions titan_cf_options;
  titan_cf_options.max_gc_batch_size = 1 << 30;
  titan_cf_options.blob_file_target_size = 256 << 20;
  NewBlobStorageAndPicker(titan_db_options, titan_cf_options);
  for (size_t i = 1; i < 9; i++) {
    // add 70 files with 10MB valid data each file
    AddBlobFile(i, titan_cf_options.blob_file_target_size, 246 << 20, false, 1);
  }
  UpdateBlobStorage();
  auto blob_gc1 = basic_blob_gc_picker_->PickBlobGC(blob_storage_.get());
  ASSERT_TRUE(blob_gc1 != nullptr);
  ASSERT_EQ(blob_gc1->trigger_next(), true);
  ASSERT_EQ(blob_gc1->inputs().size(), 4);
  auto blob_gc2 = basic_blob_gc_picker_->PickBlobGC(blob_storage_.get());
  ASSERT_TRUE(blob_gc2 != nullptr);
  ASSERT_EQ(blob_gc2->trigger_next(), false);
  ASSERT_EQ(blob_gc2->inputs().size(), 4);
  for (auto file : blob_gc1->inputs()) {
    RemoveBlobFile(file->file_number());
  }
  for (auto file : blob_gc2->inputs()) {
    RemoveBlobFile(file->file_number());
  }
  UpdateBlobStorage();
}

TEST_F(BlobGCPickerTest, NoPickGC_cold) {
  TitanDBOptions titan_db_options;
  TitanCFOptions titan_cf_options;
  titan_cf_options.min_gc_batch_size = 0;
  NewBlobStorageAndPicker(titan_db_options, titan_cf_options);
  AddBlobFile(1U, 1U, 0U, false, 1);
  UpdateBlobStorage();
  auto blob_gc = basic_blob_gc_picker_->PickBlobGC(blob_storage_.get());
  ASSERT_TRUE(blob_gc == nullptr);
}

TEST_F(BlobGCPickerTest, Small_PickGC_cold) {
  TitanDBOptions titan_db_options;
  TitanCFOptions titan_cf_options;
  titan_cf_options.min_gc_batch_size = 0;
  NewBlobStorageAndPicker(titan_db_options, titan_cf_options);
  AddBlobFile(1U, 4U << 20, 0U, false, 1);
  AddBlobFile(2U, 4U << 20, 0U, false, 1);
  UpdateBlobStorage();
  auto blob_gc = basic_blob_gc_picker_->PickBlobGC(blob_storage_.get());
  ASSERT_TRUE(blob_gc != nullptr);
  ASSERT_EQ(blob_gc->trigger_next(), false);
  ASSERT_EQ(blob_gc->inputs().size(), 2);
}

TEST_F(BlobGCPickerTest, PickGC_hot_and_cold) {
  TitanDBOptions titan_db_options;
  TitanCFOptions titan_cf_options;
  titan_cf_options.min_gc_batch_size = 0;
  NewBlobStorageAndPicker(titan_db_options, titan_cf_options);
  AddBlobFile(1U, 256U << 20, 200U << 20, false, 1);
  AddBlobFile(2U, 256U << 20, 200U << 20, false, 0);
  AddBlobFile(3U, 256U << 20, 200U << 20, false, 1);
  AddBlobFile(4U, 256U << 20, 200U << 20, false, 0);
  UpdateBlobStorage();
  auto blob_gc = basic_blob_gc_picker_->PickBlobGC(blob_storage_.get());
  ASSERT_TRUE(blob_gc != nullptr);
  ASSERT_EQ(blob_gc->trigger_next(), false);
  ASSERT_EQ(blob_gc->inputs().size(), 4);
}

}  // namespace titandb
}  // namespace rocksdb

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

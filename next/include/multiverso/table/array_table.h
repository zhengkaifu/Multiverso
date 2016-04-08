#ifndef MULTIVERSO_ARRAY_TABLE_H_
#define MULTIVERSO_ARRAY_TABLE_H_

#include "multiverso/multiverso.h"
#include "multiverso/table_interface.h"
#include "multiverso/util/log.h"
#include "multiverso/updater/updater.h"
#include "multiverso/util/configure.h"

namespace multiverso {

// A distributed shared std::vector<T> table

template <typename T>
class ArrayWorker : public WorkerTable {
public:
  explicit ArrayWorker(size_t size) : WorkerTable(), size_(size) {
    // table_.resize(size);
    num_server_ = MV_NumServers(); 
    server_offsets_.push_back(0);
    CHECK(size_ > MV_NumServers()); 
    int length = static_cast<int>(size_) / MV_NumServers();
    for (int i = 1; i < MV_NumServers(); ++i) {
      server_offsets_.push_back(i * length); // may not balance
    }
    server_offsets_.push_back(size_);
	Log::Debug("worker %d create arrayTable with %d elements.\n", MV_Rank(), size);
  }

  // std::vector<T>& raw() { return table_; }
  T* raw() { return data_; }

  // Get all element
  // data is user-allocated memory
  void Get(T* data, size_t size) {
    CHECK(size == size_);
    data_ = data;
    int all_key = -1;
    Blob whole_table(&all_key, sizeof(int));
    WorkerTable::Get(whole_table); 
    Log::Debug("worker %d getting all parameters.\n", MV_Rank());
  }

  // Add all element
  void Add(T* data, size_t size) {
    CHECK(size == size_);
    int all_key = -1;

    Blob key(&all_key, sizeof(int));
    Blob val(data, sizeof(T) * size);
    WorkerTable::Add(key, val);
    Log::Debug("worker %d adding parameters with size of %d.\n", MV_Rank(), size);
  }

  int Partition(const std::vector<Blob>& kv,
    std::unordered_map<int, std::vector<Blob> >* out) override {
    CHECK(kv.size() == 1 || kv.size() == 2);
    for (int i = 0; i < num_server_; ++i) (*out)[i].push_back(kv[0]);
    if (kv.size() == 2) {
      CHECK(kv[1].size() == size_ * sizeof(T));
      for (int i = 0; i < num_server_; ++i) {
        Blob blob(kv[1].data() + server_offsets_[i] * sizeof(T), 
          (server_offsets_[i + 1] - server_offsets_[i]) * sizeof(T));
        (*out)[i].push_back(blob);
      }
    }
    return num_server_;
  }

  void ProcessReplyGet(std::vector<Blob>& reply_data) override {
    CHECK(reply_data.size() == 2);
    int id = (reply_data[0]).As<int>();
    CHECK(reply_data[1].size<T>() == (server_offsets_[id+1] - server_offsets_[id]));

    memcpy(data_ + server_offsets_[id], reply_data[1].data(), reply_data[1].size());
  }
  
private:
  // std::vector<T> table_;
  T* data_; // not owned
  size_t size_;
  int num_server_;
  std::vector<size_t> server_offsets_;
};



// The storage is a continuous large chunk of memory
template <typename T>
class ArrayServer : public ServerTable {
public:
  explicit ArrayServer(size_t size) : ServerTable() {
    server_id_ = MV_Rank();
    size_ = size / MV_NumServers(); 
    if (server_id_ == MV_NumServers() - 1) { // last server 
      size_ += size % MV_NumServers();
    }
    storage_.resize(size_);
    updater_ = Updater<T>::GetUpdater(size_);
	  Log::Debug("server %d create arrayTable with %d elements of %d elements.\n", server_id_, size_, size);
  }

  void ProcessAdd(const std::vector<Blob>& data) override {
    Blob keys = data[0], values = data[1];
    CHECK(keys.size<int>() == 1 && keys.As<int>() == -1); // Always request whole table
    CHECK(values.size() == size_ * sizeof(T));
    T* pvalues = reinterpret_cast<T*>(values.data());
    updater_->Update(size_, storage_.data(), pvalues);
  }

  void ProcessGet(const std::vector<Blob>& data,
    std::vector<Blob>* result) override {
    size_t key_size = data[0].size<int>();
    CHECK(key_size == 1 && data[0].As<int>() == -1); // Always request the whole table
    Blob key(sizeof(int)); key.As<int>() = server_id_;
    Blob value(storage_.data(), sizeof(T) * size_);
    result->push_back(key);
    result->push_back(value);
  }

  void Store(Stream* s) override{
    s->Write(storage_.data(), storage_.size() * sizeof(T));
  }
  void Load(Stream* s) override{
    s->Read(storage_.data(), storage_.size() * sizeof(T));
  }

private:
  int server_id_;
  std::vector<T> storage_;
  Updater<T>* updater_;
  size_t size_; // number of element with type T
  
};

template<typename T>
class ArrayTableHelper : public TableHelper {
  ArrayTableHelper(const size_t& size) : size_(size) { }
protected:
  WorkerTable* CreateWorkerTable() override{
    return new ArrayWorker<T>(size_);
  }
  ServerTable* CreateServerTable() override{
    return new ArrayServer<T>(size_);
  }
  size_t size_;
};
}

#endif // MULTIVERSO_ARRAY_TABLE_H_

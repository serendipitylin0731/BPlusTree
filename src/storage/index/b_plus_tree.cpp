#include "storage/index/b_plus_tree.h"

#include <sstream>
#include <string>

#include "buffer/lru_k_replacer.h"
#include "common/config.h"
#include "common/exception.h"
#include "common/logger.h"
#include "common/macros.h"
#include "common/rid.h"
#include "storage/index/index_iterator.h"
#include "storage/page/b_plus_tree_header_page.h"
#include "storage/page/b_plus_tree_internal_page.h"
#include "storage/page/b_plus_tree_leaf_page.h"
#include "storage/page/b_plus_tree_page.h"
#include "storage/page/page_guard.h"

namespace bustub
{

INDEX_TEMPLATE_ARGUMENTS
BPLUSTREE_TYPE::BPlusTree(std::string name, page_id_t header_page_id,
                          BufferPoolManager* buffer_pool_manager,
                          const KeyComparator& comparator, int leaf_max_size,
                          int internal_max_size)
    : index_name_(std::move(name)),
      bpm_(buffer_pool_manager),
      comparator_(std::move(comparator)),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size),
      header_page_id_(header_page_id)
{
  WritePageGuard guard = bpm_ -> FetchPageWrite(header_page_id_);
  // In the original bpt, I fetch the header page
  // thus there's at least one page now
  auto root_header_page = guard.template AsMut<BPlusTreeHeaderPage>();
  // reinterprete the data of the page into "HeaderPage"
  root_header_page -> root_page_id_ = INVALID_PAGE_ID;
  // set the root_id to INVALID
}

/*
 * Helper function to decide whether current b+tree is empty
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::IsEmpty() const  ->  bool
{
  ReadPageGuard guard = bpm_ -> FetchPageRead(header_page_id_);
  auto root_header_page = guard.template As<BPlusTreeHeaderPage>();
  bool is_empty = root_header_page -> root_page_id_ == INVALID_PAGE_ID;
  // Just check if the root_page_id is INVALID
  // usage to fetch a page:
  // fetch the page guard   ->   call the "As" function of the page guard
  // to reinterprete the data of the page as "BPlusTreePage"
  return is_empty;
}
/*****************************************************************************
 * SEARCH
 *****************************************************************************/
/*
 * Return the only value that associated with input key
 * This method is used for point query
 * @return : true means key exists
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetValue(const KeyType& key,
                              std::vector<ValueType>* result, Transaction* txn)
     ->  bool
{
  ReadPageGuard head_guard = bpm_->FetchPageRead(header_page_id_);
  auto header = head_guard.template As<BPlusTreeHeaderPage>();
  if (header->root_page_id_ == INVALID_PAGE_ID) {
    return false;
  }

  ReadPageGuard guard = bpm_->FetchPageRead(header->root_page_id_);
  head_guard.Drop();

  auto page = guard.template As<BPlusTreePage>();
  while (!page->IsLeafPage()) {
    auto internal = reinterpret_cast<const InternalPage*>(page);
    int idx = BinaryFind(internal, key);
    page_id_t child_id = internal->ValueAt(idx);
    if (child_id == INVALID_PAGE_ID) {
      std::cerr << "GetValue: INVALID_PAGE_ID at internal page " << guard.PageId() << " idx " << idx << " size " << internal->GetSize() << std::endl;
      std::terminate();
    }
    guard = bpm_->FetchPageRead(child_id);
    page = guard.template As<BPlusTreePage>();
  }

  auto leaf = reinterpret_cast<const LeafPage*>(page);
  int pos = BinaryFind(leaf, key);
  if (pos != -1 && comparator_(leaf->KeyAt(pos), key) == 0) {
    result->push_back(leaf->ValueAt(pos));
    return true;
  }
  return false;
}



/*****************************************************************************
 * INSERTION
 *****************************************************************************/
/*
 * Insert constant key & value pair into b+ tree
 * if current tree is empty, start new tree, update root page id and insert
 * entry, otherwise insert into leaf page.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */


INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Insert(const KeyType& key, const ValueType& value,
                            Transaction* txn)  ->  bool
{
  Context ctx;

  ctx.header_page_ = bpm_->FetchPageWrite(header_page_id_);
  auto header = ctx.header_page_->AsMut<BPlusTreeHeaderPage>();
  ctx.root_page_id_ = header->root_page_id_;

  if (ctx.root_page_id_ == INVALID_PAGE_ID) {
    page_id_t new_page_id;
    auto new_page = bpm_->NewPage(&new_page_id);
    WritePageGuard guard(bpm_, new_page);
    auto leaf = guard.template AsMut<LeafPage>();
    leaf->Init(leaf_max_size_);
    leaf->SetKeyAt(0, key);
    leaf->SetValueAt(0, value);
    leaf->SetSize(1);
    header->root_page_id_ = new_page_id;
    return true;
  }

  auto root_guard = bpm_->FetchPageWrite(ctx.root_page_id_);
  auto root_page = root_guard.template As<BPlusTreePage>();

  if (root_page->IsLeafPage()) {
    auto leaf = root_guard.template AsMut<LeafPage>();
    int pos = BinaryFind(leaf, key);
    if (pos != -1 && comparator_(leaf->KeyAt(pos), key) == 0) {
      return false;
    }
    if (leaf->GetSize() < leaf->GetMaxSize()) {
      int insert_pos = pos + 1;
      for (int i = leaf->GetSize(); i > insert_pos; i--) {
        leaf->SetKeyAt(i, leaf->KeyAt(i - 1));
      leaf->SetValueAt(i, leaf->ValueAt(i - 1));
      }
      leaf->SetKeyAt(insert_pos, key);
      leaf->SetValueAt(insert_pos, value);
      leaf->IncreaseSize(1);
      return true;
    }
    page_id_t new_page_id;
    auto new_page = bpm_->NewPage(&new_page_id);
    WritePageGuard new_guard(bpm_, new_page);
    auto new_leaf = new_guard.template AsMut<LeafPage>();
    new_leaf->Init(leaf_max_size_);
    new_leaf->SetSize(new_leaf->GetMaxSize());

    int mid = leaf->GetSize() / 2;
    for (int i = mid; i < leaf->GetSize(); i++) {
      new_leaf->SetKeyAt(i - mid, leaf->KeyAt(i));
      new_leaf->SetValueAt(i - mid, leaf->ValueAt(i));
    }
    new_leaf->SetSize(leaf->GetSize() - mid);
    leaf->SetSize(mid);

    new_leaf->SetNextPageId(leaf->GetNextPageId());
    leaf->SetNextPageId(new_page_id);

    if (comparator_(key, new_leaf->KeyAt(0)) >= 0) {
      int insert_pos = BinaryFind(new_leaf, key) + 1;
      for (int i = new_leaf->GetSize(); i > insert_pos; i--) {
        new_leaf->SetKeyAt(i, new_leaf->KeyAt(i - 1));
      new_leaf->SetValueAt(i, new_leaf->ValueAt(i - 1));
      }
      new_leaf->SetKeyAt(insert_pos, key);
      new_leaf->SetValueAt(insert_pos, value);
      new_leaf->IncreaseSize(1);
    } else {
      int insert_pos = pos + 1;
      for (int i = leaf->GetSize(); i > insert_pos; i--) {
        leaf->SetKeyAt(i, leaf->KeyAt(i - 1));
      leaf->SetValueAt(i, leaf->ValueAt(i - 1));
      }
      leaf->SetKeyAt(insert_pos, key);
      leaf->SetValueAt(insert_pos, value);
      leaf->IncreaseSize(1);
    }

    page_id_t new_root_id;
    auto new_root_page = bpm_->NewPage(&new_root_id);
    if (new_root_page == nullptr) {
      std::terminate();
    }
    WritePageGuard root_guard_new(bpm_, new_root_page);
    auto new_root = root_guard_new.template AsMut<InternalPage>();
    new_root->Init(internal_max_size_);
    new_root->SetValueAt(0, ctx.root_page_id_);
    KeyType up_key = new_leaf->KeyAt(0);
    new_root->SetKeyAt(1, up_key);
    new_root->SetValueAt(1, new_page_id);
    new_root->SetSize(2);
    header->root_page_id_ = new_root_id;
    return true;
  }

  ctx.write_set_.push_back(std::move(root_guard));

  while (true) {
    auto internal = reinterpret_cast<const InternalPage*>(ctx.write_set_.back().template As<BPlusTreePage>());
    int idx = BinaryFind(internal, key);
    page_id_t child_id = internal->ValueAt(idx);

    auto child_guard = bpm_->FetchPageWrite(child_id);
    ctx.write_set_.push_back(std::move(child_guard));

    if (ctx.write_set_.back().template As<BPlusTreePage>()->IsLeafPage()) {
      break;
    }
  }

  auto& leaf_guard = ctx.write_set_.back();
  auto leaf = leaf_guard.template AsMut<LeafPage>();

  int pos = BinaryFind(leaf, key);
  if (pos != -1 && comparator_(leaf->KeyAt(pos), key) == 0) {
    return false;
  }

  if (leaf->GetSize() < leaf->GetMaxSize()) {
    int insert_pos = pos + 1;
    for (int i = leaf->GetSize(); i > insert_pos; i--) {
      leaf->SetKeyAt(i, leaf->KeyAt(i - 1));
      leaf->SetValueAt(i, leaf->ValueAt(i - 1));
    }
    leaf->SetKeyAt(insert_pos, key);
      leaf->SetValueAt(insert_pos, value);
    leaf->IncreaseSize(1);
    return true;
  }

  page_id_t new_page_id;
  auto new_page = bpm_->NewPage(&new_page_id);
  if (new_page == nullptr) {
    std::terminate();
  }
  WritePageGuard new_guard(bpm_, new_page);
  auto new_leaf = new_guard.template AsMut<LeafPage>();
  new_leaf->Init(leaf_max_size_);

  int mid = leaf->GetSize() / 2;
  for (int i = mid; i < leaf->GetSize(); i++) {
    new_leaf->SetKeyAt(i - mid, leaf->KeyAt(i));
      new_leaf->SetValueAt(i - mid, leaf->ValueAt(i));
  }
  new_leaf->SetSize(leaf->GetSize() - mid);
  leaf->SetSize(mid);

  new_leaf->SetNextPageId(leaf->GetNextPageId());
  leaf->SetNextPageId(new_page_id);

  if (comparator_(key, new_leaf->KeyAt(0)) >= 0) {
    int insert_pos = BinaryFind(new_leaf, key) + 1;
    for (int i = new_leaf->GetSize(); i > insert_pos; i--) {
      new_leaf->SetKeyAt(i, new_leaf->KeyAt(i - 1));
      new_leaf->SetValueAt(i, new_leaf->ValueAt(i - 1));
    }
    new_leaf->SetKeyAt(insert_pos, key);
      new_leaf->SetValueAt(insert_pos, value);
    new_leaf->IncreaseSize(1);
  } else {
    int insert_pos = pos + 1;
    for (int i = leaf->GetSize(); i > insert_pos; i--) {
      leaf->SetKeyAt(i, leaf->KeyAt(i - 1));
      leaf->SetValueAt(i, leaf->ValueAt(i - 1));
    }
    leaf->SetKeyAt(insert_pos, key);
      leaf->SetValueAt(insert_pos, value);
    leaf->IncreaseSize(1);
  }

  KeyType up_key = new_leaf->KeyAt(0);
  page_id_t up_value = new_page_id;
  ctx.write_set_.pop_back();

  while (!ctx.write_set_.empty()) {
    auto& parent_guard = ctx.write_set_.back();
    auto parent = parent_guard.template AsMut<InternalPage>();

    if (parent->GetSize() < parent->GetMaxSize()) {
      int insert_pos = parent->GetSize();
      while (insert_pos > 1 && comparator_(parent->KeyAt(insert_pos - 1), up_key) > 0) {
        parent->SetKeyAt(insert_pos, parent->KeyAt(insert_pos - 1));
        parent->SetValueAt(insert_pos, parent->ValueAt(insert_pos - 1));
        insert_pos--;
      }
      parent->SetKeyAt(insert_pos, up_key);
      parent->SetValueAt(insert_pos, up_value);
      parent->IncreaseSize(1);
      return true;
    }

    page_id_t new_internal_id;
    auto new_internal_page = bpm_->NewPage(&new_internal_id);
    if (new_internal_page == nullptr) {
      std::terminate();
    }
    WritePageGuard split_guard(bpm_, new_internal_page);
    auto new_internal = split_guard.template AsMut<InternalPage>();
    new_internal->Init(internal_max_size_);

    int split_mid = parent->GetSize() / 2;
    new_internal->SetValueAt(0, parent->ValueAt(split_mid));
    for (int i = split_mid + 1; i < parent->GetSize(); i++) {
      new_internal->SetKeyAt(i - split_mid, parent->KeyAt(i));
      new_internal->SetValueAt(i - split_mid, parent->ValueAt(i));
    }
    new_internal->SetSize(parent->GetSize() - split_mid);

    KeyType up_key_parent = parent->KeyAt(split_mid);
    parent->SetSize(split_mid);

    if (comparator_(up_key, up_key_parent) >= 0) {
      int insert_pos = new_internal->GetSize();
      while (insert_pos > 1 && comparator_(new_internal->KeyAt(insert_pos - 1), up_key) > 0) {
        new_internal->SetKeyAt(insert_pos, new_internal->KeyAt(insert_pos - 1));
        new_internal->SetValueAt(insert_pos, new_internal->ValueAt(insert_pos - 1));
        insert_pos--;
      }
      new_internal->SetKeyAt(insert_pos, up_key);
      new_internal->SetValueAt(insert_pos, up_value);
      new_internal->IncreaseSize(1);
    } else {
      int insert_pos = parent->GetSize();
      while (insert_pos > 1 && comparator_(parent->KeyAt(insert_pos - 1), up_key) > 0) {
        parent->SetKeyAt(insert_pos, parent->KeyAt(insert_pos - 1));
        parent->SetValueAt(insert_pos, parent->ValueAt(insert_pos - 1));
        insert_pos--;
      }
      parent->SetKeyAt(insert_pos, up_key);
      parent->SetValueAt(insert_pos, up_value);
      parent->IncreaseSize(1);
    }

    up_key = up_key_parent;
    up_value = new_internal_id;
    ctx.write_set_.pop_back();
  }

    page_id_t new_root_id;
    auto new_root_page = bpm_->NewPage(&new_root_id);
    if (new_root_page == nullptr) {
      std::terminate();
    }
    WritePageGuard new_root_guard(bpm_, new_root_page);
  auto new_root = new_root_guard.template AsMut<InternalPage>();
  new_root->Init(internal_max_size_);
  new_root->SetValueAt(0, ctx.root_page_id_);
  new_root->SetKeyAt(1, up_key);
  new_root->SetValueAt(1, up_value);
  new_root->SetSize(2);
  header->root_page_id_ = new_root_id;
  return true;
}


/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * Delete key & value pair associated with input key
 * If current tree is empty, return immediately.
 * If not, User needs to first find the right leaf page as deletion target, then
 * delete entry from leaf page. Remember to deal with redistribute or merge if
 * necessary.
 */

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Remove(const KeyType& key, Transaction* txn)
{
  auto RemoveFromInternal = [](InternalPage* page, int idx) {
    for (int i = idx; i < page->GetSize() - 1; i++) {
      page->SetValueAt(i, page->ValueAt(i + 1));
      page->SetKeyAt(i, page->KeyAt(i + 1));
    }
    page->IncreaseSize(-1);
    KeyType invalid;
    invalid.SetFromInteger(0);
    page->SetKeyAt(0, invalid);
  };

  Context ctx;

  ctx.header_page_ = bpm_->FetchPageWrite(header_page_id_);
  auto header = ctx.header_page_->AsMut<BPlusTreeHeaderPage>();
  ctx.root_page_id_ = header->root_page_id_;

  if (ctx.root_page_id_ == INVALID_PAGE_ID) {
    return;
  }

  auto root_guard = bpm_->FetchPageWrite(ctx.root_page_id_);
  auto root_page = root_guard.template As<BPlusTreePage>();

  if (root_page->IsLeafPage()) {
    auto leaf = root_guard.template AsMut<LeafPage>();
    int pos = BinaryFind(leaf, key);
    if (pos == -1 || comparator_(leaf->KeyAt(pos), key) != 0) {
      return;
    }
    for (int i = pos; i < leaf->GetSize() - 1; i++) {
      leaf->SetKeyAt(i, leaf->KeyAt(i + 1));
      leaf->SetValueAt(i, leaf->ValueAt(i + 1));
    }
    leaf->IncreaseSize(-1);
    if (leaf->GetSize() == 0) {
      header->root_page_id_ = INVALID_PAGE_ID;
    }
    return;
  }

  ctx.write_set_.push_back(std::move(root_guard));

  while (true) {
    auto internal = reinterpret_cast<const InternalPage*>(ctx.write_set_.back().template As<BPlusTreePage>());
    int idx = BinaryFind(internal, key);
    page_id_t child_id = internal->ValueAt(idx);

    auto child_guard = bpm_->FetchPageWrite(child_id);
    ctx.write_set_.push_back(std::move(child_guard));

    if (ctx.write_set_.back().template As<BPlusTreePage>()->IsLeafPage()) {
      break;
    }
  }

  auto& leaf_guard = ctx.write_set_.back();
  auto leaf = leaf_guard.template AsMut<LeafPage>();

  int pos = BinaryFind(leaf, key);
  if (pos == -1 || comparator_(leaf->KeyAt(pos), key) != 0) {
    return;
  }

  for (int i = pos; i < leaf->GetSize() - 1; i++) {
    leaf->SetKeyAt(i, leaf->KeyAt(i + 1));
      leaf->SetValueAt(i, leaf->ValueAt(i + 1));
  }
  leaf->IncreaseSize(-1);

  if (leaf->GetSize() >= leaf->GetMinSize()) {
    if (pos == 0 && leaf->GetSize() > 0 && ctx.write_set_.size() > 1) {
      auto& parent_guard = ctx.write_set_[ctx.write_set_.size() - 2];
      auto parent = parent_guard.template AsMut<InternalPage>();
      for (int i = 0; i < parent->GetSize(); i++) {
        if (parent->ValueAt(i) == leaf_guard.PageId()) {
          parent->SetKeyAt(i, leaf->KeyAt(0));
          break;
        }
      }
    }
    return;
  }

  if (ctx.write_set_.size() <= 1) {
    return;
  }

  auto& parent_guard = ctx.write_set_[ctx.write_set_.size() - 2];
  auto parent = parent_guard.template AsMut<InternalPage>();

  int leaf_idx = -1;
  for (int i = 0; i < parent->GetSize(); i++) {
    if (parent->ValueAt(i) == leaf_guard.PageId()) {
      leaf_idx = i;
      break;
    }
  }

  if (leaf_idx == -1) {
    return;
  }

  if (leaf_idx > 0) {
    auto left_guard = bpm_->FetchPageWrite(parent->ValueAt(leaf_idx - 1));
    auto left_leaf = left_guard.template AsMut<LeafPage>();
    if (left_leaf->GetSize() > left_leaf->GetMinSize()) {
      int borrow_idx = left_leaf->GetSize() - 1;
      for (int i = leaf->GetSize(); i > 0; i--) {
        leaf->SetKeyAt(i, leaf->KeyAt(i - 1));
      leaf->SetValueAt(i, leaf->ValueAt(i - 1));
      }
      leaf->SetKeyAt(0, left_leaf->KeyAt(borrow_idx));
      leaf->SetValueAt(0, left_leaf->ValueAt(borrow_idx));
      leaf->IncreaseSize(1);
      left_leaf->IncreaseSize(-1);
      parent->SetKeyAt(leaf_idx, leaf->KeyAt(0));
      return;
    }
  }

  if (leaf_idx < parent->GetSize() - 1) {
    auto right_guard = bpm_->FetchPageWrite(parent->ValueAt(leaf_idx + 1));
    auto right_leaf = right_guard.template AsMut<LeafPage>();
    if (right_leaf->GetSize() > right_leaf->GetMinSize()) {
      leaf->SetKeyAt(leaf->GetSize(), right_leaf->KeyAt(0));
      leaf->SetValueAt(leaf->GetSize(), right_leaf->ValueAt(0));
      leaf->IncreaseSize(1);
      for (int i = 0; i < right_leaf->GetSize() - 1; i++) {
        right_leaf->SetKeyAt(i, right_leaf->KeyAt(i + 1));
      right_leaf->SetValueAt(i, right_leaf->ValueAt(i + 1));
      }
      right_leaf->IncreaseSize(-1);
      parent->SetKeyAt(leaf_idx + 1, right_leaf->KeyAt(0));
      return;
    }
  }

  if (leaf_idx > 0) {
    auto left_guard = bpm_->FetchPageWrite(parent->ValueAt(leaf_idx - 1));
    auto left_leaf = left_guard.template AsMut<LeafPage>();
    for (int i = 0; i < leaf->GetSize(); i++) {
      left_leaf->SetKeyAt(left_leaf->GetSize() + i, leaf->KeyAt(i));
      left_leaf->SetValueAt(left_leaf->GetSize() + i, leaf->ValueAt(i));
    }
    left_leaf->SetSize(left_leaf->GetSize() + leaf->GetSize());
    left_leaf->SetNextPageId(leaf->GetNextPageId());
    RemoveFromInternal(parent, leaf_idx);
  } else {
    auto right_guard = bpm_->FetchPageWrite(parent->ValueAt(leaf_idx + 1));
    auto right_leaf = right_guard.template AsMut<LeafPage>();
    for (int i = 0; i < right_leaf->GetSize(); i++) {
      leaf->SetKeyAt(leaf->GetSize() + i, right_leaf->KeyAt(i));
      leaf->SetValueAt(leaf->GetSize() + i, right_leaf->ValueAt(i));
    }
    leaf->SetSize(leaf->GetSize() + right_leaf->GetSize());
    leaf->SetNextPageId(right_leaf->GetNextPageId());
    RemoveFromInternal(parent, leaf_idx + 1);
  }

  ctx.write_set_.pop_back();

  while (!ctx.write_set_.empty()) {
    auto& node_guard = ctx.write_set_.back();
    auto node = node_guard.template AsMut<InternalPage>();

    if (node->GetSize() >= node->GetMinSize()) {
      break;
    }

    if (ctx.write_set_.size() == 1) {
      if (node->GetSize() == 1) {
        header->root_page_id_ = node->ValueAt(0);
      }
      break;
    }

    auto& grandparent_guard = ctx.write_set_[ctx.write_set_.size() - 2];
    auto grandparent = grandparent_guard.template AsMut<InternalPage>();

    int node_idx = -1;
    for (int i = 0; i < grandparent->GetSize(); i++) {
      if (grandparent->ValueAt(i) == node_guard.PageId()) {
        node_idx = i;
        break;
      }
    }

    if (node_idx == -1) {
      break;
    }

    if (node_idx > 0) {
      auto left_guard = bpm_->FetchPageWrite(grandparent->ValueAt(node_idx - 1));
      auto left_node = left_guard.template AsMut<InternalPage>();
      if (left_node->GetSize() > left_node->GetMinSize()) {
        KeyType separator = grandparent->KeyAt(node_idx);
        int last_idx = left_node->GetSize() - 1;
        for (int i = node->GetSize(); i > 0; i--) {
          node->SetKeyAt(i, node->KeyAt(i - 1));
          node->SetValueAt(i, node->ValueAt(i - 1));
        }
        node->SetValueAt(0, left_node->ValueAt(last_idx));
        node->SetKeyAt(1, separator);
        node->IncreaseSize(1);

        grandparent->SetKeyAt(node_idx, left_node->KeyAt(last_idx));
        left_node->IncreaseSize(-1);
        break;
      }
    }

    if (node_idx < grandparent->GetSize() - 1) {
      auto right_guard = bpm_->FetchPageWrite(grandparent->ValueAt(node_idx + 1));
      auto right_node = right_guard.template AsMut<InternalPage>();
      if (right_node->GetSize() > right_node->GetMinSize()) {
        KeyType separator = grandparent->KeyAt(node_idx + 1);
        node->SetKeyAt(node->GetSize(), separator);
        node->SetValueAt(node->GetSize(), right_node->ValueAt(0));
        node->IncreaseSize(1);

        grandparent->SetKeyAt(node_idx + 1, right_node->KeyAt(1));

        right_node->SetValueAt(0, right_node->ValueAt(1));
        for (int i = 1; i < right_node->GetSize() - 1; i++) {
          right_node->SetKeyAt(i, right_node->KeyAt(i + 1));
          right_node->SetValueAt(i, right_node->ValueAt(i + 1));
        }
        right_node->IncreaseSize(-1);
        KeyType invalid;
        invalid.SetFromInteger(0);
        right_node->SetKeyAt(0, invalid);
        break;
      }
    }

    if (node_idx > 0) {
      auto left_guard = bpm_->FetchPageWrite(grandparent->ValueAt(node_idx - 1));
      auto left_node = left_guard.template AsMut<InternalPage>();
      KeyType separator = grandparent->KeyAt(node_idx);
      left_node->SetKeyAt(left_node->GetSize(), separator);
      left_node->SetValueAt(left_node->GetSize(), node->ValueAt(0));
      left_node->IncreaseSize(1);
      for (int i = 1; i < node->GetSize(); i++) {
        left_node->SetKeyAt(left_node->GetSize(), node->KeyAt(i));
        left_node->SetValueAt(left_node->GetSize(), node->ValueAt(i));
        left_node->IncreaseSize(1);
      }
      RemoveFromInternal(grandparent, node_idx);
    } else {
      auto right_guard = bpm_->FetchPageWrite(grandparent->ValueAt(node_idx + 1));
      auto right_node = right_guard.template AsMut<InternalPage>();
      KeyType separator = grandparent->KeyAt(node_idx + 1);
      node->SetKeyAt(node->GetSize(), separator);
      node->SetValueAt(node->GetSize(), right_node->ValueAt(0));
      node->IncreaseSize(1);
      for (int i = 1; i < right_node->GetSize(); i++) {
        node->SetKeyAt(node->GetSize(), right_node->KeyAt(i));
        node->SetValueAt(node->GetSize(), right_node->ValueAt(i));
        node->IncreaseSize(1);
      }
      RemoveFromInternal(grandparent, node_idx + 1);
    }

    ctx.write_set_.pop_back();
  }
}

/*****************************************************************************
 * INDEX ITERATOR
 *****************************************************************************/


INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::BinaryFind(const LeafPage* leaf_page, const KeyType& key)
     ->  int
{
  int l = 0;
  int r = leaf_page -> GetSize() - 1;
  while (l < r)
  {
    int mid = (l + r + 1) >> 1;
    if (comparator_(leaf_page -> KeyAt(mid), key) != 1)
    {
      l = mid;
    }
    else
    {
      r = mid - 1;
    }
  }

  if (r >= 0 && comparator_(leaf_page -> KeyAt(r), key) == 1)
  {
    r = -1;
  }

  return r;
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::BinaryFind(const InternalPage* internal_page,
                                const KeyType& key)  ->  int
{
  int l = 1;
  int r = internal_page -> GetSize() - 1;
  while (l < r)
  {
    int mid = (l + r + 1) >> 1;
    if (comparator_(internal_page -> KeyAt(mid), key) != 1)
    {
      l = mid;
    }
    else
    {
      r = mid - 1;
    }
  }

  if (r == -1 || comparator_(internal_page -> KeyAt(r), key) == 1)
  {
    r = 0;
  }

  return r;
}

/*
 * Input parameter is void, find the leftmost leaf page first, then construct
 * index iterator
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Begin()  ->  INDEXITERATOR_TYPE
//Just go left forever
{
  ReadPageGuard head_guard = bpm_ -> FetchPageRead(header_page_id_);
  if (head_guard.template As<BPlusTreeHeaderPage>() -> root_page_id_ == INVALID_PAGE_ID)
  {
    return End();
  }
  ReadPageGuard guard = bpm_ -> FetchPageRead(head_guard.template As<BPlusTreeHeaderPage>() -> root_page_id_);
  head_guard.Drop();

  auto tmp_page = guard.template As<BPlusTreePage>();
  while (!tmp_page -> IsLeafPage())
  {
    int slot_num = 0;
    guard = bpm_ -> FetchPageRead(reinterpret_cast<const InternalPage*>(tmp_page) -> ValueAt(slot_num));
    tmp_page = guard.template As<BPlusTreePage>();
  }
  int slot_num = 0;
  if (slot_num != -1)
  {
    return INDEXITERATOR_TYPE(bpm_, guard.PageId(), 0);
  }
  return End();
}


/*
 * Input parameter is low key, find the leaf page that contains the input key
 * first, then construct index iterator
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Begin(const KeyType& key)  ->  INDEXITERATOR_TYPE
{
  ReadPageGuard head_guard = bpm_ -> FetchPageRead(header_page_id_);

  if (head_guard.template As<BPlusTreeHeaderPage>() -> root_page_id_ == INVALID_PAGE_ID)
  {
    return End();
  }
  ReadPageGuard guard = bpm_ -> FetchPageRead(head_guard.template As<BPlusTreeHeaderPage>() -> root_page_id_);
  head_guard.Drop();
  auto tmp_page = guard.template As<BPlusTreePage>();
  while (!tmp_page -> IsLeafPage())
  {
    auto internal = reinterpret_cast<const InternalPage*>(tmp_page);
    int slot_num = BinaryFind(internal, key);
    if (slot_num == -1)
    {
      return End();
    }
    guard = bpm_ -> FetchPageRead(reinterpret_cast<const InternalPage*>(tmp_page) -> ValueAt(slot_num));
    tmp_page = guard.template As<BPlusTreePage>();
  }
  auto* leaf_page = reinterpret_cast<const LeafPage*>(tmp_page);

  int slot_num = BinaryFind(leaf_page, key);
  if (slot_num != -1)
  {
    return INDEXITERATOR_TYPE(bpm_, guard.PageId(), slot_num);
  }
  return End();
}

/*
 * Input parameter is void, construct an index iterator representing the end
 * of the key/value pair in the leaf node
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::End()  ->  INDEXITERATOR_TYPE
{
  return INDEXITERATOR_TYPE(bpm_, -1, -1);
}

/**
 * @return Page id of the root of this tree
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetRootPageId()  ->  page_id_t
{
  ReadPageGuard guard = bpm_ -> FetchPageRead(header_page_id_);
  auto root_header_page = guard.template As<BPlusTreeHeaderPage>();
  page_id_t root_page_id = root_header_page -> root_page_id_;
  return root_page_id;
}

/*****************************************************************************
 * UTILITIES AND DEBUG
 *****************************************************************************/

/*
 * This method is used for test only
 * Read data from file and insert one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertFromFile(const std::string& file_name,
                                    Transaction* txn)
{
  int64_t key;
  std::ifstream input(file_name);
  while (input >> key)
  {
    KeyType index_key;
    index_key.SetFromInteger(key);
    RID rid(key);
    Insert(index_key, rid, txn);
  }
}
/*
 * This method is used for test only
 * Read data from file and remove one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::RemoveFromFile(const std::string& file_name,
                                    Transaction* txn)
{
  int64_t key;
  std::ifstream input(file_name);
  while (input >> key)
  {
    KeyType index_key;
    index_key.SetFromInteger(key);
    Remove(index_key, txn);
  }
}

/*
 * This method is used for test only
 * Read data from file and insert/remove one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::BatchOpsFromFile(const std::string& file_name,
                                      Transaction* txn)
{
  int64_t key;
  char instruction;
  std::ifstream input(file_name);
  while (input)
  {
    input >> instruction >> key;
    RID rid(key);
    KeyType index_key;
    index_key.SetFromInteger(key);
    switch (instruction)
    {
      case 'i':
        Insert(index_key, rid, txn);
        break;
      case 'd':
        Remove(index_key, txn);
        break;
      default:
        break;
    }
  }
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Print(BufferPoolManager* bpm)
{
  auto root_page_id = GetRootPageId();
  auto guard = bpm -> FetchPageBasic(root_page_id);
  PrintTree(guard.PageId(), guard.template As<BPlusTreePage>());
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::PrintTree(page_id_t page_id, const BPlusTreePage* page)
{
  if (page -> IsLeafPage())
  {
    auto* leaf = reinterpret_cast<const LeafPage*>(page);
    std::cout << "Leaf Page: " << page_id << "\tNext: " << leaf -> GetNextPageId() << std::endl;

    // Print the contents of the leaf page.
    std::cout << "Contents: ";
    for (int i = 0; i < leaf -> GetSize(); i++)
    {
      std::cout << leaf -> KeyAt(i);
      if ((i + 1) < leaf -> GetSize())
      {
        std::cout << ", ";
      }
    }
    std::cout << std::endl;
    std::cout << std::endl;
  }
  else
  {
    auto* internal = reinterpret_cast<const InternalPage*>(page);
    std::cout << "Internal Page: " << page_id << std::endl;

    // Print the contents of the internal page.
    std::cout << "Contents: ";
    for (int i = 0; i < internal -> GetSize(); i++)
    {
      std::cout << internal -> KeyAt(i) << ": " << internal -> ValueAt(i);
      if ((i + 1) < internal -> GetSize())
      {
        std::cout << ", ";
      }
    }
    std::cout << std::endl;
    std::cout << std::endl;
    for (int i = 0; i < internal -> GetSize(); i++)
    {
      auto guard = bpm_ -> FetchPageBasic(internal -> ValueAt(i));
      PrintTree(guard.PageId(), guard.template As<BPlusTreePage>());
    }
  }
}

/**
 * This method is used for debug only, You don't need to modify
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Draw(BufferPoolManager* bpm, const std::string& outf)
{
  if (IsEmpty())
  {
    LOG_WARN("Drawing an empty tree");
    return;
  }

  std::ofstream out(outf);
  out << "digraph G {" << std::endl;
  auto root_page_id = GetRootPageId();
  auto guard = bpm -> FetchPageBasic(root_page_id);
  ToGraph(guard.PageId(), guard.template As<BPlusTreePage>(), out);
  out << "}" << std::endl;
  out.close();
}

/**
 * This method is used for debug only, You don't need to modify
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::ToGraph(page_id_t page_id, const BPlusTreePage* page,
                             std::ofstream& out)
{
  std::string leaf_prefix("LEAF_");
  std::string internal_prefix("INT_");
  if (page -> IsLeafPage())
  {
    auto* leaf = reinterpret_cast<const LeafPage*>(page);
    // Print node name
    out << leaf_prefix << page_id;
    // Print node properties
    out << "[shape=plain color=green ";
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" "
           "CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << leaf -> GetSize() << "\">P=" << page_id
        << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << leaf -> GetSize() << "\">"
        << "max_size=" << leaf -> GetMaxSize()
        << ",min_size=" << leaf -> GetMinSize() << ",size=" << leaf -> GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < leaf -> GetSize(); i++)
    {
      out << "<TD>" << leaf -> KeyAt(i) << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Leaf node link if there is a next page
    if (leaf -> GetNextPageId() != INVALID_PAGE_ID)
    {
      out << leaf_prefix << page_id << "   ->   " << leaf_prefix
          << leaf -> GetNextPageId() << ";\n";
      out << "{rank=same " << leaf_prefix << page_id << " " << leaf_prefix
          << leaf -> GetNextPageId() << "};\n";
    }
  }
  else
  {
    auto* inner = reinterpret_cast<const InternalPage*>(page);
    // Print node name
    out << internal_prefix << page_id;
    // Print node properties
    out << "[shape=plain color=pink ";  // why not?
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" "
           "CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << inner -> GetSize() << "\">P=" << page_id
        << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << inner -> GetSize() << "\">"
        << "max_size=" << inner -> GetMaxSize()
        << ",min_size=" << inner -> GetMinSize() << ",size=" << inner -> GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < inner -> GetSize(); i++)
    {
      out << "<TD PORT=\"p" << inner -> ValueAt(i) << "\">";
      // if (i > 0) {
      out << inner -> KeyAt(i) << "  " << inner -> ValueAt(i);
      // } else {
      // out << inner  ->  ValueAt(0);
      // }
      out << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print leaves
    for (int i = 0; i < inner -> GetSize(); i++)
    {
      auto child_guard = bpm_ -> FetchPageBasic(inner -> ValueAt(i));
      auto child_page = child_guard.template As<BPlusTreePage>();
      ToGraph(child_guard.PageId(), child_page, out);
      if (i > 0)
      {
        auto sibling_guard = bpm_ -> FetchPageBasic(inner -> ValueAt(i - 1));
        auto sibling_page = sibling_guard.template As<BPlusTreePage>();
        if (!sibling_page -> IsLeafPage() && !child_page -> IsLeafPage())
        {
          out << "{rank=same " << internal_prefix << sibling_guard.PageId()
              << " " << internal_prefix << child_guard.PageId() << "};\n";
        }
      }
      out << internal_prefix << page_id << ":p" << child_guard.PageId()
          << "   ->   ";
      if (child_page -> IsLeafPage())
      {
        out << leaf_prefix << child_guard.PageId() << ";\n";
      }
      else
      {
        out << internal_prefix << child_guard.PageId() << ";\n";
      }
    }
  }
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::DrawBPlusTree()  ->  std::string
{
  if (IsEmpty())
  {
    return "()";
  }

  PrintableBPlusTree p_root = ToPrintableBPlusTree(GetRootPageId());
  std::ostringstream out_buf;
  p_root.Print(out_buf);

  return out_buf.str();
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::ToPrintableBPlusTree(page_id_t root_id)
     ->  PrintableBPlusTree
{
  auto root_page_guard = bpm_ -> FetchPageBasic(root_id);
  auto root_page = root_page_guard.template As<BPlusTreePage>();
  PrintableBPlusTree proot;

  if (root_page -> IsLeafPage())
  {
    auto leaf_page = root_page_guard.template As<LeafPage>();
    proot.keys_ = leaf_page -> ToString();
    proot.size_ = proot.keys_.size() + 4;  // 4 more spaces for indent

    return proot;
  }

  // draw internal page
  auto internal_page = root_page_guard.template As<InternalPage>();
  proot.keys_ = internal_page -> ToString();
  proot.size_ = 0;
  for (int i = 0; i < internal_page -> GetSize(); i++)
  {
    page_id_t child_id = internal_page -> ValueAt(i);
    PrintableBPlusTree child_node = ToPrintableBPlusTree(child_id);
    proot.size_ += child_node.size_;
    proot.children_.push_back(child_node);
  }

  return proot;
}

template class BPlusTree<GenericKey<4>, RID, GenericComparator<4>>;

template class BPlusTree<GenericKey<8>, RID, GenericComparator<8>>;

template class BPlusTree<GenericKey<16>, RID, GenericComparator<16>>;

template class BPlusTree<GenericKey<32>, RID, GenericComparator<32>>;

template class BPlusTree<GenericKey<64>, RID, GenericComparator<64>>;

}  // namespace bustub
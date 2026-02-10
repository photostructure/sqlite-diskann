/*
** DiskANN Node Binary Format & Graph Helpers
**
** Derived from libSQL DiskANN implementation
** Original Copyright 2024 the libSQL authors
** Modifications Copyright 2025 PhotoStructure Inc.
** MIT License
*/
#include "diskann_node.h"
#include <assert.h>
#include <math.h>
#include <sqlite3.h>
#include <stdlib.h>

/**************************************************************************
** Layout calculation
**************************************************************************/

uint32_t node_edges_max_count(const DiskAnnIndex *idx) {
  uint32_t node_overhead =
      NODE_METADATA_SIZE + idx->nNodeVectorSize;
  uint32_t edge_overhead =
      idx->nEdgeVectorSize + EDGE_METADATA_SIZE;
  assert(idx->block_size >= node_overhead);
  return (idx->block_size - node_overhead) / edge_overhead;
}

uint32_t node_edges_metadata_offset(const DiskAnnIndex *idx) {
  uint32_t max_edges = node_edges_max_count(idx);
  uint32_t offset = NODE_METADATA_SIZE + idx->nNodeVectorSize +
                    max_edges * idx->nEdgeVectorSize;
  assert(offset <= idx->block_size);
  return offset;
}

/**************************************************************************
** Node binary operations
**************************************************************************/

void node_bin_init(const DiskAnnIndex *idx, BlobSpot *spot, uint64_t rowid,
                   const float *vector) {
  assert(NODE_METADATA_SIZE + idx->nNodeVectorSize <= spot->buffer_size);

  memset(spot->buffer, 0, spot->buffer_size);
  write_le64(spot->buffer, rowid);
  /* Edge count is zero after memset — no need to write explicitly */

  memcpy(spot->buffer + NODE_METADATA_SIZE, vector, idx->nNodeVectorSize);
}

const float *node_bin_vector(const DiskAnnIndex *idx, const BlobSpot *spot) {
  assert(NODE_METADATA_SIZE + idx->nNodeVectorSize <= spot->buffer_size);

  return (const float *)(spot->buffer + NODE_METADATA_SIZE);
}

uint16_t node_bin_edges(const DiskAnnIndex *idx, const BlobSpot *spot) {
  assert(NODE_METADATA_SIZE <= spot->buffer_size);
  (void)idx;

  return read_le16(spot->buffer + sizeof(uint64_t));
}

void node_bin_edge(const DiskAnnIndex *idx, const BlobSpot *spot, int edge_idx,
                   uint64_t *rowid, float *distance, const float **vector) {
  uint32_t meta_offset = node_edges_metadata_offset(idx);

  if (rowid != NULL) {
    assert(meta_offset +
               (uint32_t)(edge_idx + 1) * EDGE_METADATA_SIZE <=
           spot->buffer_size);
    *rowid = read_le64(spot->buffer + meta_offset +
                       (uint32_t)edge_idx * EDGE_METADATA_SIZE +
                       sizeof(uint64_t));
  }
  if (distance != NULL) {
    uint32_t raw = read_le32(spot->buffer + meta_offset +
                             (uint32_t)edge_idx * EDGE_METADATA_SIZE +
                             sizeof(uint32_t));
    memcpy(distance, &raw, sizeof(float));
  }
  if (vector != NULL) {
    uint32_t vec_offset = NODE_METADATA_SIZE + idx->nNodeVectorSize +
                          (uint32_t)edge_idx * idx->nEdgeVectorSize;
    assert(vec_offset + idx->nEdgeVectorSize <= meta_offset);
    *vector = (const float *)(spot->buffer + vec_offset);
  }
}

int node_bin_edge_find_idx(const DiskAnnIndex *idx, const BlobSpot *spot,
                           uint64_t rowid) {
  int n_edges = node_bin_edges(idx, spot);
  for (int i = 0; i < n_edges; i++) {
    uint64_t edge_id;
    node_bin_edge(idx, spot, i, &edge_id, NULL, NULL);
    if (edge_id == rowid) {
      return i;
    }
  }
  return -1;
}

void node_bin_replace_edge(const DiskAnnIndex *idx, BlobSpot *spot,
                           int replace_idx, uint64_t rowid, float distance,
                           const float *vector) {
  uint32_t max_edges = node_edges_max_count(idx);
  uint16_t n_edges = node_bin_edges(idx, spot);
  uint32_t meta_offset = node_edges_metadata_offset(idx);

  assert(replace_idx >= 0 && (uint32_t)replace_idx < max_edges);
  assert(replace_idx >= 0 && replace_idx <= n_edges);

  if (replace_idx == n_edges) {
    n_edges++;
  }

  uint32_t edge_vec_offset = NODE_METADATA_SIZE + idx->nNodeVectorSize +
                             (uint32_t)replace_idx * idx->nEdgeVectorSize;
  uint32_t edge_meta_offset =
      meta_offset + (uint32_t)replace_idx * EDGE_METADATA_SIZE;

  assert(edge_vec_offset + idx->nEdgeVectorSize <= spot->buffer_size);
  assert(edge_meta_offset + EDGE_METADATA_SIZE <= spot->buffer_size);

  /* Write edge vector */
  memcpy(spot->buffer + edge_vec_offset, vector, idx->nEdgeVectorSize);

  /* Write edge metadata: [padding(4)] [distance(4)] [rowid(8)] */
  uint32_t dist_raw;
  memcpy(&dist_raw, &distance, sizeof(float));
  write_le32(spot->buffer + edge_meta_offset + sizeof(uint32_t), dist_raw);
  write_le64(spot->buffer + edge_meta_offset + sizeof(uint64_t), rowid);

  /* Update edge count */
  write_le16(spot->buffer + sizeof(uint64_t), n_edges);
}

void node_bin_delete_edge(const DiskAnnIndex *idx, BlobSpot *spot,
                          int delete_idx) {
  uint16_t n_edges = node_bin_edges(idx, spot);
  uint32_t meta_offset = node_edges_metadata_offset(idx);

  assert(delete_idx >= 0 && delete_idx < n_edges);

  uint32_t del_vec = NODE_METADATA_SIZE + idx->nNodeVectorSize +
                     (uint32_t)delete_idx * idx->nEdgeVectorSize;
  uint32_t last_vec = NODE_METADATA_SIZE + idx->nNodeVectorSize +
                      (uint32_t)(n_edges - 1) * idx->nEdgeVectorSize;
  uint32_t del_meta =
      meta_offset + (uint32_t)delete_idx * EDGE_METADATA_SIZE;
  uint32_t last_meta =
      meta_offset + (uint32_t)(n_edges - 1) * EDGE_METADATA_SIZE;

  /* Swap with last edge if not already the last */
  if (del_vec < last_vec) {
    memmove(spot->buffer + del_vec, spot->buffer + last_vec,
            idx->nEdgeVectorSize);
    memmove(spot->buffer + del_meta, spot->buffer + last_meta,
            EDGE_METADATA_SIZE);
  }

  /* Decrement edge count */
  write_le16(spot->buffer + sizeof(uint64_t),
             (uint16_t)(n_edges - 1));
}

void node_bin_prune_edges(const DiskAnnIndex *idx, BlobSpot *spot,
                          int n_pruned) {
  assert(n_pruned >= 0 && n_pruned <= node_bin_edges(idx, spot));
  (void)idx;

  write_le16(spot->buffer + sizeof(uint64_t), (uint16_t)n_pruned);
}

/**************************************************************************
** Distance functions
**************************************************************************/

float diskann_distance_l2(const float *a, const float *b, uint32_t dims) {
  float sum = 0.0f;
  for (uint32_t i = 0; i < dims; i++) {
    float diff = a[i] - b[i];
    sum += diff * diff;
  }
  return sum;
}

float diskann_distance_cosine(const float *a, const float *b, uint32_t dims) {
  float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
  for (uint32_t i = 0; i < dims; i++) {
    dot += a[i] * b[i];
    norm_a += a[i] * a[i];
    norm_b += b[i] * b[i];
  }
  float denom = sqrtf(norm_a) * sqrtf(norm_b);
  if (denom == 0.0f) {
    return 0.0f;
  }
  return 1.0f - dot / denom;
}

float diskann_distance(const float *a, const float *b, uint32_t dims,
                       uint8_t metric) {
  switch (metric) {
  case DISKANN_METRIC_EUCLIDEAN:
    return diskann_distance_l2(a, b, dims);
  case DISKANN_METRIC_COSINE:
    return diskann_distance_cosine(a, b, dims);
  default:
    assert(0 && "unsupported distance metric");
    return 0.0f;
  }
}

/**************************************************************************
** Buffer management
**************************************************************************/

int distance_buffer_insert_idx(const float *distances, int size, int max_size,
                               float distance) {
  for (int i = 0; i < size; i++) {
    if (distance < distances[i]) {
      return i;
    }
  }
  return size < max_size ? size : -1;
}

void buffer_insert(uint8_t *buf, int size, int max_size, int insert_idx,
                   int item_size, const uint8_t *item, uint8_t *last) {
  assert(max_size > 0 && item_size > 0);
  assert(size <= max_size);
  assert(0 <= insert_idx && insert_idx <= size && insert_idx < max_size);

  if (size == max_size) {
    if (last != NULL) {
      memcpy(last, buf + (size - 1) * item_size, (size_t)item_size);
    }
    size--;
  }
  int items_to_move = size - insert_idx;
  memmove(buf + (insert_idx + 1) * item_size, buf + insert_idx * item_size,
          (size_t)items_to_move * (size_t)item_size);
  memcpy(buf + insert_idx * item_size, item, (size_t)item_size);
}

void buffer_delete(uint8_t *buf, int size, int delete_idx, int item_size) {
  assert(item_size > 0);
  assert(0 <= delete_idx && delete_idx < size);

  int items_to_move = size - delete_idx - 1;
  memmove(buf + delete_idx * item_size, buf + (delete_idx + 1) * item_size,
          (size_t)items_to_move * (size_t)item_size);
}

/**************************************************************************
** Node allocation
**************************************************************************/

DiskAnnNode *diskann_node_alloc(uint64_t rowid) {
  DiskAnnNode *node = (DiskAnnNode *)sqlite3_malloc(sizeof(DiskAnnNode));
  if (node == NULL) {
    return NULL;
  }
  node->rowid = rowid;
  node->visited = 0;
  node->next = NULL;
  node->blob_spot = NULL;
  return node;
}

void diskann_node_free(DiskAnnNode *node) {
  if (node == NULL) {
    return;
  }
  if (node->blob_spot != NULL) {
    blob_spot_free(node->blob_spot);
  }
  sqlite3_free(node);
}

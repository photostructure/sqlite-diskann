/*
** DiskANN Node Binary Format & Graph Helpers
**
** Derived from libSQL DiskANN implementation
** Original Copyright 2024 the libSQL authors
** Modifications Copyright 2026 PhotoStructure Inc.
** MIT License
**
** This module provides:
** - Little-endian serialization (read/write LE16/32/64)
** - Node BLOB binary format (init, read vector, edge CRUD)
** - Layout calculation (metadata size, max edges, offsets)
** - Distance functions (L2, cosine)
** - Buffer management (sorted insert/delete)
** - Node alloc/free
*/
#ifndef DISKANN_NODE_H
#define DISKANN_NODE_H

#include "diskann_blob.h"
#include "diskann_internal.h"
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
** V3 format constants (only format we support)
**
** Node metadata: 16 bytes (u64 rowid + u64 edge count in low 16 bits)
** Edge metadata: 16 bytes (4b padding + 4b distance + 8b rowid)
*/
#define NODE_METADATA_SIZE 16
#define EDGE_METADATA_SIZE 16

/**************************************************************************
** Little-endian serialization (inline for performance)
**************************************************************************/

static inline uint16_t read_le16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static inline uint32_t read_le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}

static inline uint64_t read_le64(const uint8_t *p) {
  return (uint64_t)p[0] | (uint64_t)p[1] << 8 | (uint64_t)p[2] << 16 |
         (uint64_t)p[3] << 24 | (uint64_t)p[4] << 32 | (uint64_t)p[5] << 40 |
         (uint64_t)p[6] << 48 | (uint64_t)p[7] << 56;
}

static inline void write_le16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v);
  p[1] = (uint8_t)(v >> 8);
}

static inline void write_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v);
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static inline void write_le64(uint8_t *p, uint64_t v) {
  p[0] = (uint8_t)(v);
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
  p[4] = (uint8_t)(v >> 32);
  p[5] = (uint8_t)(v >> 40);
  p[6] = (uint8_t)(v >> 48);
  p[7] = (uint8_t)(v >> 56);
}

/**************************************************************************
** DiskAnnNode — represents a single node in the graph
**************************************************************************/

typedef struct DiskAnnNode DiskAnnNode;

struct DiskAnnNode {
  uint64_t rowid;      /* Node ID */
  int visited;         /* Has this node been visited during search? */
  DiskAnnNode *next;   /* Next node in visited list (linked list) */
  BlobSpot *blob_spot; /* BLOB handle for node data (NULL if not loaded) */
};

/**************************************************************************
** Layout calculation helpers
**
** Node BLOB layout (V3, float32-only):
**   [0..15]   Node metadata: rowid(8) + edge_count(8, low 16 bits used)
**   [16..]    Node vector: dims * sizeof(float)
**   [..]      Edge vectors: max_edges * nEdgeVectorSize
**   [..]      Edge metadata: max_edges * EDGE_METADATA_SIZE
**************************************************************************/

/*
** Calculate max number of edges that fit in a block.
** Returns 0 if block is too small (caller must check).
*/
uint32_t node_edges_max_count(const DiskAnnIndex *idx);

/*
** Calculate the byte offset where edge metadata begins.
** Edge metadata is NOT contiguous with edge vectors — it starts after
** ALL edge vector slots (even unused ones).
*/
uint32_t node_edges_metadata_offset(const DiskAnnIndex *idx);

/**************************************************************************
** Node binary operations — read/write node BLOBs
**
** All functions operate on BlobSpot buffers. The caller is responsible
** for creating/loading/flushing BlobSpots via the BLOB I/O layer.
**************************************************************************/

/*
** Initialize a node BLOB: write rowid, zero edge count, copy vector data.
** Clears the entire buffer first (zero-fills unused space).
*/
void node_bin_init(const DiskAnnIndex *idx, BlobSpot *spot, uint64_t rowid,
                   const float *vector);

/*
** Get a read-only pointer to the node's vector data (zero-copy).
** Returns pointer into the BlobSpot buffer — valid until buffer changes.
*/
const float *node_bin_vector(const DiskAnnIndex *idx, const BlobSpot *spot);

/*
** Read edge count from node BLOB.
*/
uint16_t node_bin_edges(const DiskAnnIndex *idx, const BlobSpot *spot);

/*
** Read edge at index. Any output parameter can be NULL if not needed.
** - rowid: target node ID
** - distance: distance to target (float stored as LE u32)
** - vector: pointer to edge vector data (zero-copy into buffer)
*/
void node_bin_edge(const DiskAnnIndex *idx, const BlobSpot *spot, int edge_idx,
                   uint64_t *rowid, float *distance, const float **vector);

/*
** Find edge index by target rowid. Returns -1 if not found.
*/
int node_bin_edge_find_idx(const DiskAnnIndex *idx, const BlobSpot *spot,
                           uint64_t rowid);

/*
** Replace edge at position, or append if position == edge_count.
** Copies vector data and writes metadata.
*/
void node_bin_replace_edge(const DiskAnnIndex *idx, BlobSpot *spot,
                           int replace_idx, uint64_t rowid, float distance,
                           const float *vector);

/*
** Delete edge by swapping with the last edge, then decrementing count.
*/
void node_bin_delete_edge(const DiskAnnIndex *idx, BlobSpot *spot,
                          int delete_idx);

/*
** Truncate edge list to n_pruned edges.
*/
void node_bin_prune_edges(const DiskAnnIndex *idx, BlobSpot *spot,
                          int n_pruned);

/**************************************************************************
** Distance functions
**************************************************************************/

/*
** L2 (squared Euclidean) distance between two float32 vectors.
*/
float diskann_distance_l2(const float *a, const float *b, uint32_t dims);

/*
** Cosine distance: 1.0 - cosine_similarity.
** Returns 0.0 for identical directions, 2.0 for opposite.
*/
float diskann_distance_cosine(const float *a, const float *b, uint32_t dims);

/*
** Dispatch distance calculation by metric type.
*/
float diskann_distance(const float *a, const float *b, uint32_t dims,
                       uint8_t metric);

/**************************************************************************
** Buffer management — sorted array insert/delete
**************************************************************************/

/*
** Find insertion index for a distance value in a sorted (ascending) array.
** Returns the index where `distance` should be inserted, or -1 if the
** array is full and distance is larger than all existing entries.
*/
int distance_buffer_insert_idx(const float *distances, int size, int max_size,
                               float distance);

/*
** Insert item at position in a buffer, shifting subsequent items right.
** If buffer is full, the last item is evicted (copied to `last` if non-NULL).
*/
void buffer_insert(uint8_t *buf, int size, int max_size, int insert_idx,
                   int item_size, const uint8_t *item, uint8_t *last);

/*
** Delete item at position, shifting subsequent items left.
*/
void buffer_delete(uint8_t *buf, int size, int delete_idx, int item_size);

/**************************************************************************
** Node allocation
**************************************************************************/

/*
** Allocate a new DiskAnnNode with the given rowid.
** Initializes: visited=0, next=NULL, blob_spot=NULL.
** Returns NULL on allocation failure.
*/
DiskAnnNode *diskann_node_alloc(uint64_t rowid);

/*
** Free a DiskAnnNode and its associated BlobSpot (if any).
*/
void diskann_node_free(DiskAnnNode *node);

#ifdef __cplusplus
}
#endif

#endif /* DISKANN_NODE_H */

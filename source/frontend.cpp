#include "mcut/internal/frontend.h"
#include "mcut/internal/preproc.h"

#include "mcut/internal/hmesh.h"
#include "mcut/internal/math.h"
#include "mcut/internal/utils.h"

#include <algorithm>
#include <array>
#include <fstream>

#include <memory>

#include <stdio.h>
#include <string.h>
#include <unordered_map>
#include <numeric> // iota

#include "mcut/internal/cdt/cdt.h"

#if defined(MCUT_MULTI_THREADED)
#include "mcut/internal/tpool.h"
std::atomic_bool thread_pool_terminate(false);
#endif

#if defined(PROFILING_BUILD)
std::stack<std::unique_ptr<mini_timer>> g_timestack = std::stack<std::unique_ptr<mini_timer>>();
#endif

std::map<McContext, std::unique_ptr<context_t>> g_contexts = {};

void create_context_impl(McContext* pOutContext, McFlags flags)
{
    MCUT_ASSERT(pOutContext != nullptr);

    // allocate internal context object (including associated threadpool etc.)
    std::unique_ptr<context_t> context_uptr = std::unique_ptr<context_t>(new context_t());

    // copy context configuration flags
    context_uptr->flags = flags;

    // create handle (ptr) which will be returned and used by client to access rest of API
    const McContext handle = reinterpret_cast<McContext>(context_uptr.get());

    const std::pair<std::map<McContext, std::unique_ptr<context_t>>::iterator, bool> insertion_result = g_contexts.emplace(handle, std::move(context_uptr));

    const bool context_inserted_ok = insertion_result.second;

    if (!context_inserted_ok) {
        throw std::runtime_error("failed to create context");
    }

    const std::map<McContext, std::unique_ptr<context_t>>::iterator context_entry_iter = insertion_result.first;

    MCUT_ASSERT(handle == context_entry_iter->first);

    *pOutContext = context_entry_iter->first;
}

void debug_message_callback_impl(
    McContext contextHandle,
    pfn_mcDebugOutput_CALLBACK cb,
    const void* userParam)
{
    MCUT_ASSERT(contextHandle != nullptr);
    MCUT_ASSERT(cb != nullptr);

    std::map<McContext, std::unique_ptr<context_t>>::iterator context_entry_iter = g_contexts.find(contextHandle);

    if (context_entry_iter == g_contexts.end()) {
        // "contextHandle" may not be NULL but that does not mean it maps to
        // a valid object in "g_contexts"
        throw std::invalid_argument("invalid context");
    }

    const std::unique_ptr<context_t>& context_uptr = context_entry_iter->second;

    // set callback function ptr, and user pointer
    context_uptr->debugCallback = cb;
    context_uptr->debugCallbackUserParam = userParam;
}

// find the number of trailing zeros in v
// http://graphics.stanford.edu/~seander/bithacks.html#ZerosOnRightLinear
int trailing_zeroes(unsigned int v)
{
    int r; // the result goes here
#ifdef _WIN32
#pragma warning(disable : 4146) // "unary minus operator applied to unsigned type, result still unsigned"
#endif // #ifdef _WIN32
    float f = (float)(v & -v); // cast the least significant bit in v to a float
#ifdef _WIN32
#pragma warning(default : 4146)
#endif // #ifdef _WIN32

// dereferencing type-punned pointer will break strict-aliasing rules
#if __linux__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#endif

    r = (*(uint32_t*)&f >> 23) - 0x7f;

#if __linux__
#pragma GCC diagnostic pop
#endif
    return r;
}

// https://stackoverflow.com/questions/47981/how-do-you-set-clear-and-toggle-a-single-bit
int set_bit(unsigned int v, unsigned int pos)
{
    v |= 1U << pos;
    return v;
}

int clear_bit(unsigned int v, unsigned int pos)
{
    v &= ~(1UL << pos);
    return v;
}

void debug_message_control_impl(
    McContext contextHandle,
    McDebugSource source,
    McDebugType type,
    McDebugSeverity severity,
    bool enabled)
{
    std::map<McContext, std::unique_ptr<context_t>>::iterator context_entry_iter = g_contexts.find(contextHandle);

    if (context_entry_iter == g_contexts.end()) {
        throw std::invalid_argument("invalid context");
    }

    const std::unique_ptr<context_t>& context_uptr = context_entry_iter->second;

    // reset
    context_uptr->debugSource = 0;

    for (auto i : { MC_DEBUG_SOURCE_API, MC_DEBUG_SOURCE_KERNEL }) {
        if ((source & i) && enabled) {

            int n = trailing_zeroes(MC_DEBUG_SOURCE_ALL & i);

            context_uptr->debugSource = set_bit(context_uptr->debugSource, n);
        }
    }

    // reset
    context_uptr->debugType = 0;

    for (auto i : { MC_DEBUG_TYPE_DEPRECATED_BEHAVIOR, MC_DEBUG_TYPE_ERROR, MC_DEBUG_TYPE_OTHER }) {
        if ((type & i) && enabled) {

            int n = trailing_zeroes(MC_DEBUG_TYPE_ALL & i);

            context_uptr->debugType = set_bit(context_uptr->debugType, n);
        }
    }

    // reset
    context_uptr->debugSeverity = 0;

    for (auto i : { MC_DEBUG_SEVERITY_HIGH, MC_DEBUG_SEVERITY_LOW, MC_DEBUG_SEVERITY_MEDIUM, MC_DEBUG_SEVERITY_NOTIFICATION }) {
        if ((severity & i) && enabled) {

            int n = trailing_zeroes(MC_DEBUG_SEVERITY_ALL & i);

            context_uptr->debugSeverity = set_bit(context_uptr->debugSeverity, n);
        }
    }
}

void get_info_impl(
    const McContext context,
    McFlags info,
    uint64_t bytes,
    void* pMem,
    uint64_t* pNumBytes)
{
    std::map<McContext, std::unique_ptr<context_t>>::iterator context_entry_iter = g_contexts.find(context);

    if (context_entry_iter == g_contexts.end()) {
        throw std::invalid_argument("invalid context");
    }

    const std::unique_ptr<context_t>& context_uptr = context_entry_iter->second;

    switch (info) {
    case MC_CONTEXT_FLAGS:
        if (pMem == nullptr) {
            *pNumBytes = sizeof(context_uptr->flags);
        } else {
            memcpy(pMem, reinterpret_cast<void*>(&context_uptr->flags), bytes);
        }
        break;
    default:
        throw std::invalid_argument("unknown info parameter");
        break;
    }
}

void dispatch_impl(
    McContext context,
    McFlags flags,
    const void* pSrcMeshVertices,
    const uint32_t* pSrcMeshFaceIndices,
    const uint32_t* pSrcMeshFaceSizes,
    uint32_t numSrcMeshVertices,
    uint32_t numSrcMeshFaces,
    const void* pCutMeshVertices,
    const uint32_t* pCutMeshFaceIndices,
    const uint32_t* pCutMeshFaceSizes,
    uint32_t numCutMeshVertices,
    uint32_t numCutMeshFaces)
{
    std::map<McContext, std::unique_ptr<context_t>>::iterator context_entry_iter = g_contexts.find(context);

    if (context_entry_iter == g_contexts.end()) {
        throw std::invalid_argument("invalid context");
    }

    std::unique_ptr<context_t>& context_uptr = context_entry_iter->second;

    context_uptr->dispatchFlags = flags;

    preproc(
        context_uptr,
        pSrcMeshVertices,
        pSrcMeshFaceIndices,
        pSrcMeshFaceSizes,
        numSrcMeshVertices,
        numSrcMeshFaces,
        pCutMeshVertices,
        pCutMeshFaceIndices,
        pCutMeshFaceSizes,
        numCutMeshVertices,
        numCutMeshFaces);
}

void get_connected_components_impl(
    const McContext context,
    const McConnectedComponentType connectedComponentType,
    const uint32_t numEntries,
    McConnectedComponent* pConnComps,
    uint32_t* numConnComps)
{
    std::map<McContext, std::unique_ptr<context_t>>::iterator context_entry_iter = g_contexts.find(context);

    if (context_entry_iter == g_contexts.end()) {
        throw std::invalid_argument("invalid context");
    }

    const std::unique_ptr<context_t>& context_uptr = context_entry_iter->second;

    if (numConnComps != nullptr) {
        (*numConnComps) = 0; // reset
    }

    uint32_t valid_cc_counter = 0;

    for (std::map<McConnectedComponent, std::unique_ptr<connected_component_t, void (*)(connected_component_t*)>>::const_iterator i = context_uptr->connected_components.cbegin();
         i != context_uptr->connected_components.cend();
         ++i) {

        const bool is_valid = (i->second->type & connectedComponentType) != 0;

        if (is_valid) {
            if (pConnComps == nullptr) // query number
            {
                (*numConnComps)++;
            } else // populate pConnComps
            {
                pConnComps[valid_cc_counter] = i->first;
                valid_cc_counter += 1;
                if (valid_cc_counter == numEntries) {
                    break;
                }
            }
        }
    }
}

void get_connected_component_data_impl(
    const McContext context,
    const McConnectedComponent connCompId,
    McFlags flags,
    uint64_t bytes,
    void* pMem,
    uint64_t* pNumBytes)
{

    std::map<McContext, std::unique_ptr<context_t>>::iterator context_entry_iter = g_contexts.find(context);

    if (context_entry_iter == g_contexts.end()) {
        throw std::invalid_argument("invalid context");
    }

    const std::unique_ptr<context_t>& context_uptr = context_entry_iter->second;

    std::map<McConnectedComponent, std::unique_ptr<connected_component_t, void (*)(connected_component_t*)>>::const_iterator cc_entry_iter = context_uptr->connected_components.find(connCompId);

    if (cc_entry_iter == context_uptr->connected_components.cend()) {
        throw std::invalid_argument("invalid connected component");
    }

    const std::unique_ptr<connected_component_t, void (*)(connected_component_t*)>& cc_uptr = cc_entry_iter->second;

    switch (flags) {

    case MC_CONNECTED_COMPONENT_DATA_VERTEX_FLOAT: {
        const uint64_t allocated_bytes = cc_uptr->kernel_hmesh_data.mesh.number_of_vertices() * sizeof(float) * 3ul; // cc_uptr->indexArrayMesh.numVertices * sizeof(float) * 3;

        if (pMem == nullptr) {
            *pNumBytes = allocated_bytes;
        } else { // copy mem to client ptr

            if (bytes > allocated_bytes) {
                throw std::invalid_argument("out of bounds memory access");
            } // if

            // an element is a component
            const uint64_t nelems = (uint64_t)(bytes / sizeof(float));

            if (nelems % 3 != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }

            const uint64_t num_vertices_to_copy = (nelems / 3);
            uint64_t elem_offset = 0;
            float* casted_ptr = reinterpret_cast<float*>(pMem);

            for (vertex_array_iterator_t viter = cc_uptr->kernel_hmesh_data.mesh.vertices_begin(); viter != cc_uptr->kernel_hmesh_data.mesh.vertices_end(); ++viter) {
                const vec3& coords = cc_uptr->kernel_hmesh_data.mesh.vertex(*viter);

                for (int i = 0; i < 3; ++i) {
                    const float val = static_cast<float>(coords[i]);
                    *(casted_ptr + elem_offset) = val;
                    elem_offset += 1;
                }

                if ((elem_offset / 3) == num_vertices_to_copy) {
                    break;
                }
            }

            MCUT_ASSERT((elem_offset * sizeof(float)) <= allocated_bytes);
        }
    } break;
    case MC_CONNECTED_COMPONENT_DATA_VERTEX_DOUBLE: {
        const uint64_t allocated_bytes = cc_uptr->kernel_hmesh_data.mesh.number_of_vertices() * sizeof(double) * 3ul; // cc_uptr->indexArrayMesh.numVertices * sizeof(float) * 3;

        if (pMem == nullptr) {
            *pNumBytes = allocated_bytes;
        } else { // copy mem to client ptr

            if (bytes > allocated_bytes) {
                throw std::invalid_argument("out of bounds memory access");
            } // if

            // an element is a component
            const int64_t nelems = (uint64_t)(bytes / sizeof(double));

            if (nelems % 3 != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }

            const uint64_t num_vertices_to_copy = (nelems / 3);
            uint64_t elem_offset = 0;
            double* casted_ptr = reinterpret_cast<double*>(pMem);

            for (vertex_array_iterator_t viter = cc_uptr->kernel_hmesh_data.mesh.vertices_begin(); viter != cc_uptr->kernel_hmesh_data.mesh.vertices_end(); ++viter) {

                const vec3& coords = cc_uptr->kernel_hmesh_data.mesh.vertex(*viter);

                for (int i = 0; i < 3; ++i) {
                    *(casted_ptr + elem_offset) = coords[i];
                    elem_offset += 1;
                }

                if ((elem_offset / 3) == num_vertices_to_copy) {
                    break;
                }
            }

            MCUT_ASSERT((elem_offset * sizeof(float)) <= allocated_bytes);
        }
    } break;
    case MC_CONNECTED_COMPONENT_DATA_FACE: {
        if (pMem == nullptr) {
            uint32_t num_indices = 0;

            // TODO: make parallel
            for (face_array_iterator_t fiter = cc_uptr->kernel_hmesh_data.mesh.faces_begin(); fiter != cc_uptr->kernel_hmesh_data.mesh.faces_end(); ++fiter) {
                const uint32_t num_vertices_around_face = cc_uptr->kernel_hmesh_data.mesh.get_num_vertices_around_face(*fiter);

                MCUT_ASSERT(num_vertices_around_face >= 3);

                num_indices += num_vertices_around_face;
            }

            MCUT_ASSERT(num_indices >= 3); // min is a triangle

            *pNumBytes = num_indices * sizeof(uint32_t);
        } else {
            if (bytes % sizeof(uint32_t) != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }

            uint32_t num_indices = 0;

            std::vector<vd_t> cc_face_vertices;

            uint64_t elem_offset = 0;
            uint32_t* casted_ptr = reinterpret_cast<uint32_t*>(pMem);

            // TODO: make parallel
            for (face_array_iterator_t fiter = cc_uptr->kernel_hmesh_data.mesh.faces_begin(); fiter != cc_uptr->kernel_hmesh_data.mesh.faces_end(); ++fiter) {

                cc_face_vertices.clear();
                cc_uptr->kernel_hmesh_data.mesh.get_vertices_around_face(cc_face_vertices, *fiter);
                const uint32_t num_vertices_around_face = (uint32_t)cc_face_vertices.size();

                MCUT_ASSERT(num_vertices_around_face >= 3u);

                for (uint32_t i = 0; i < num_vertices_around_face; ++i) {
                    const uint32_t vertex_idx = (uint32_t)cc_face_vertices[i];
                    *(casted_ptr + elem_offset) = vertex_idx;
                    ++elem_offset;
                }

                num_indices += num_vertices_around_face;
            }
        }
    } break;
    case MC_CONNECTED_COMPONENT_DATA_FACE_SIZE: { // non-triangulated only (don't want to store redundant information)
        if (pMem == nullptr) {
            *pNumBytes = cc_uptr->kernel_hmesh_data.mesh.number_of_faces() * sizeof(uint32_t); // each face has a size (num verts)
        } else {
            if (bytes > cc_uptr->kernel_hmesh_data.mesh.number_of_faces() * sizeof(uint32_t)) {
                throw std::invalid_argument("out of bounds memory access");
            }

            if (bytes % sizeof(uint32_t) != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }

            uint64_t elem_offset = 0;
            uint32_t* casted_ptr = reinterpret_cast<uint32_t*>(pMem);

            // TODO: make parallel
            for (face_array_iterator_t fiter = cc_uptr->kernel_hmesh_data.mesh.faces_begin(); fiter != cc_uptr->kernel_hmesh_data.mesh.faces_end(); ++fiter) {
                const uint32_t num_vertices_around_face = cc_uptr->kernel_hmesh_data.mesh.get_num_vertices_around_face(*fiter);

                MCUT_ASSERT(num_vertices_around_face >= 3);

                *(casted_ptr + elem_offset) = num_vertices_around_face;
                ++elem_offset;
            }
        }
    } break;
    case MC_CONNECTED_COMPONENT_DATA_FACE_ADJACENT_FACE: {
        if (pMem == nullptr) {

            MCUT_ASSERT(pNumBytes != nullptr);

            uint32_t num_face_adjacent_face_indices = 0;

            // TODO: make parallel
            for (face_array_iterator_t fiter = cc_uptr->kernel_hmesh_data.mesh.faces_begin(); fiter != cc_uptr->kernel_hmesh_data.mesh.faces_end(); ++fiter) {
                const uint32_t num_faces_around_face = cc_uptr->kernel_hmesh_data.mesh.get_num_faces_around_face(*fiter, nullptr);
                num_face_adjacent_face_indices += num_faces_around_face;
            }

            *pNumBytes = num_face_adjacent_face_indices * sizeof(uint32_t);
        } else {

            if (bytes % sizeof(uint32_t) != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }

            uint64_t elem_offset = 0;
            uint32_t* casted_ptr = reinterpret_cast<uint32_t*>(pMem);

            std::vector<fd_t> faces_around_face;

            // TODO: make parallel
            for (face_array_iterator_t fiter = cc_uptr->kernel_hmesh_data.mesh.faces_begin(); fiter != cc_uptr->kernel_hmesh_data.mesh.faces_end(); ++fiter) {

                faces_around_face.clear();
                cc_uptr->kernel_hmesh_data.mesh.get_faces_around_face(faces_around_face, *fiter, nullptr);

                if (!faces_around_face.empty()) {
                    for (uint32_t i = 0; i < (uint32_t)faces_around_face.size(); ++i) {
                        *(casted_ptr + elem_offset) = (uint32_t)faces_around_face[i];
                        elem_offset++;
                    }
                }
            }

            MCUT_ASSERT((elem_offset * sizeof(uint32_t)) <= bytes);
        }
    } break;
    case MC_CONNECTED_COMPONENT_DATA_FACE_ADJACENT_FACE_SIZE: {
        if (pMem == nullptr) {
            *pNumBytes = cc_uptr->kernel_hmesh_data.mesh.number_of_faces() * sizeof(uint32_t); // each face has a size value (num adjacent faces)
        } else {
            if (bytes > cc_uptr->kernel_hmesh_data.mesh.number_of_faces() * sizeof(uint32_t)) {
                throw std::invalid_argument("out of bounds memory access");
            }

            if (bytes % sizeof(uint32_t) != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }

            uint64_t elem_offset = 0;
            uint32_t* casted_ptr = reinterpret_cast<uint32_t*>(pMem);

            // TODO: make parallel
            for (face_array_iterator_t fiter = cc_uptr->kernel_hmesh_data.mesh.faces_begin(); fiter != cc_uptr->kernel_hmesh_data.mesh.faces_end(); ++fiter) {
                const uint32_t num_faces_around_face = cc_uptr->kernel_hmesh_data.mesh.get_num_faces_around_face(*fiter, nullptr);
                *(casted_ptr + elem_offset) = num_faces_around_face;
                elem_offset++;
            }

            MCUT_ASSERT((elem_offset * sizeof(uint32_t)) <= bytes);
        }
    } break;

    case MC_CONNECTED_COMPONENT_DATA_EDGE: {
        if (pMem == nullptr) {
            *pNumBytes = cc_uptr->kernel_hmesh_data.mesh.number_of_edges() * 2 * sizeof(uint32_t); // each edge has two indices
        } else {
            if (bytes > cc_uptr->kernel_hmesh_data.mesh.number_of_edges() * 2 * sizeof(uint32_t)) {
                throw std::invalid_argument("out of bounds memory access");
            }

            if (bytes % (sizeof(uint32_t) * 2) != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }

            uint64_t elem_offset = 0;
            uint32_t* casted_ptr = reinterpret_cast<uint32_t*>(pMem);

            // TODO: make parallel
            for (edge_array_iterator_t eiter = cc_uptr->kernel_hmesh_data.mesh.edges_begin(); eiter != cc_uptr->kernel_hmesh_data.mesh.edges_end(); ++eiter) {
                const vertex_descriptor_t v0 = cc_uptr->kernel_hmesh_data.mesh.vertex(*eiter, 0);
                *(casted_ptr + elem_offset) = (uint32_t)v0;
                elem_offset++;

                const vertex_descriptor_t v1 = cc_uptr->kernel_hmesh_data.mesh.vertex(*eiter, 1);
                *(casted_ptr + elem_offset) = (uint32_t)v1;
                elem_offset++;
            }

            MCUT_ASSERT((elem_offset * sizeof(uint32_t)) <= bytes);
        }
    } break;
    case MC_CONNECTED_COMPONENT_DATA_TYPE: {
        if (pMem == nullptr) {
            *pNumBytes = sizeof(McConnectedComponentType);
        } else {
            if (bytes > sizeof(McConnectedComponentType)) {
                throw std::invalid_argument("out of bounds memory access");
            }
            if (bytes % sizeof(McConnectedComponentType) != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }
            memcpy(pMem, reinterpret_cast<void*>(&cc_uptr->type), bytes);
        }
    } break;
    case MC_CONNECTED_COMPONENT_DATA_FRAGMENT_LOCATION: {

        if (cc_uptr->type != MC_CONNECTED_COMPONENT_TYPE_FRAGMENT) {
            throw std::invalid_argument("invalid client pointer type");
        }

        if (pMem == nullptr) {
            *pNumBytes = sizeof(McFragmentLocation);
        } else {

            if (bytes > sizeof(McFragmentLocation)) {
                throw std::invalid_argument("out of bounds memory access");
            }

            if (bytes % sizeof(McFragmentLocation) != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }

            fragment_cc_t* fragPtr = dynamic_cast<fragment_cc_t*>(cc_uptr.get());
            memcpy(pMem, reinterpret_cast<void*>(&fragPtr->fragmentLocation), bytes);
        }
    } break;
    case MC_CONNECTED_COMPONENT_DATA_PATCH_LOCATION: {

        if (cc_uptr->type != MC_CONNECTED_COMPONENT_TYPE_FRAGMENT && cc_uptr->type != MC_CONNECTED_COMPONENT_TYPE_PATCH) {
            throw std::invalid_argument("connected component must be a patch or a fragment");
        }

        if (pMem == nullptr) {
            *pNumBytes = sizeof(McPatchLocation);
        } else {
            if (bytes > sizeof(McPatchLocation)) {
                throw std::invalid_argument("out of bounds memory access");
            }

            if (bytes % sizeof(McPatchLocation) != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }

            const void* src = nullptr;
            if (cc_uptr->type == MC_CONNECTED_COMPONENT_TYPE_FRAGMENT) {
                src = reinterpret_cast<const void*>(&dynamic_cast<fragment_cc_t*>(cc_uptr.get())->patchLocation);
            } else {
                MCUT_ASSERT(cc_uptr->type == MC_CONNECTED_COMPONENT_TYPE_PATCH);
                src = reinterpret_cast<const void*>(&dynamic_cast<patch_cc_t*>(cc_uptr.get())->patchLocation);
            }
            memcpy(pMem, src, bytes);
        }
    } break;
    case MC_CONNECTED_COMPONENT_DATA_FRAGMENT_SEAL_TYPE: {

        if (cc_uptr->type != MC_CONNECTED_COMPONENT_TYPE_FRAGMENT) {
            throw std::invalid_argument("invalid client pointer type");
        }

        if (pMem == nullptr) {
            *pNumBytes = sizeof(McFragmentSealType);
        } else {
            if (bytes > sizeof(McFragmentSealType)) {
                throw std::invalid_argument("out of bounds memory access");
            }

            if (bytes % sizeof(McFragmentSealType) != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }
            fragment_cc_t* fragPtr = dynamic_cast<fragment_cc_t*>(cc_uptr.get());
            memcpy(pMem, reinterpret_cast<void*>(&fragPtr->srcMeshSealType), bytes);
        }
    } break;
        //
    case MC_CONNECTED_COMPONENT_DATA_ORIGIN: {

        if (cc_uptr->type != MC_CONNECTED_COMPONENT_TYPE_SEAM && cc_uptr->type != MC_CONNECTED_COMPONENT_TYPE_INPUT) {
            throw std::invalid_argument("invalid connected component type");
        }

        size_t nbytes = (cc_uptr->type != MC_CONNECTED_COMPONENT_TYPE_SEAM ? sizeof(McSeamOrigin) : sizeof(McInputOrigin));

        if (pMem == nullptr) {
            *pNumBytes = nbytes;
        } else {
            if (bytes > nbytes) {
                throw std::invalid_argument("out of bounds memory access");
            }

            if ((bytes % nbytes) != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }

            if (cc_uptr->type == MC_CONNECTED_COMPONENT_TYPE_SEAM) {
                seam_cc_t* ptr = dynamic_cast<seam_cc_t*>(cc_uptr.get());
                memcpy(pMem, reinterpret_cast<void*>(&ptr->origin), bytes);
            } else {
                input_cc_t* ptr = dynamic_cast<input_cc_t*>(cc_uptr.get());
                memcpy(pMem, reinterpret_cast<void*>(&ptr->origin), bytes);
            }
        }
    } break;
    case MC_CONNECTED_COMPONENT_DATA_SEAM_VERTEX: {
        if (cc_uptr->type == MC_CONNECTED_COMPONENT_TYPE_INPUT) {
            throw std::invalid_argument("cannot query seam vertices on input connected component");
        }

        const uint32_t seam_vertex_count = (uint32_t)cc_uptr->kernel_hmesh_data.seam_vertices.size();

        if (pMem == nullptr) {
            *pNumBytes = seam_vertex_count * sizeof(uint32_t);
        } else {
            if (bytes > (seam_vertex_count * sizeof(uint32_t))) {
                throw std::invalid_argument("out of bounds memory access");
            }

            if ((bytes % (sizeof(uint32_t))) != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }

            const uint32_t elems_to_copy = bytes / sizeof(uint32_t);
            uint32_t elem_offset = 0;
            uint32_t* casted_ptr = reinterpret_cast<uint32_t*>(pMem);

            // TODO: make parallel
            for (uint32_t i = 0; i < elems_to_copy; ++i) {
                const uint32_t seam_vertex_idx = cc_uptr->kernel_hmesh_data.seam_vertices[i];
                *(casted_ptr + elem_offset) = seam_vertex_idx;
                elem_offset++;
            }

            MCUT_ASSERT(elem_offset <= seam_vertex_count);
        }
    } break;
    case MC_CONNECTED_COMPONENT_DATA_VERTEX_MAP: {

        const uint32_t vertex_map_size = cc_uptr->kernel_hmesh_data.data_maps.vertex_map.size();

        if (vertex_map_size == 0) {
            throw std::invalid_argument("vertex map not available"); // user probably forgot to set the dispatch flag
        }

        MCUT_ASSERT(vertex_map_size == (uint32_t)cc_uptr->kernel_hmesh_data.mesh.number_of_vertices());

        if (pMem == nullptr) {
            *pNumBytes = (vertex_map_size * sizeof(uint32_t)); // each each vertex has a map value (intersection point == uint_max)
        } else {
            if (bytes > (vertex_map_size * sizeof(uint32_t))) {
                throw std::invalid_argument("out of bounds memory access");
            }

            if (bytes % (sizeof(uint32_t)) != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }

            const uint32_t elems_to_copy = (bytes / sizeof(uint32_t));

            MCUT_ASSERT(elems_to_copy <= vertex_map_size);

            uint32_t elem_offset = 0;
            uint32_t* casted_ptr = reinterpret_cast<uint32_t*>(pMem);

            // TODO: make parallel
            for (uint32_t i = 0; i < elems_to_copy; ++i) // ... for each vertex in CC
            {
                // Here we use whatever index value was assigned to the current vertex by the kernel, where the
                // the kernel does not necessarilly know that the input meshes it was given where modified by
                // the frontend (in this case via polygon partitioning)
                // Vertices that are polygon intersection points have a value of uint_max i.e. null_vertex().

                uint32_t internal_input_mesh_vertex_idx = cc_uptr->kernel_hmesh_data.data_maps.vertex_map[i];
                // We use the same default value as that used by the kernel for intersection
                // points (intersection points at mapped to uint_max i.e. null_vertex())
                uint32_t client_input_mesh_vertex_idx = UINT32_MAX;
                // This is true only for polygon intersection points computed by the kernel
                const bool internal_input_mesh_vertex_is_intersection_point = (internal_input_mesh_vertex_idx == UINT32_MAX);

                if (!internal_input_mesh_vertex_is_intersection_point) { // i.e. a client-mesh vertex or vertex that is added due to face-partitioning
                    // NOTE: The kernel will assign/map a 'proper' index value to vertices that exist due to face partitioning.
                    // 'proper' here means that the kernel treats these vertices as 'original vertices' from a client-provided input
                    // mesh. In reality, the frontend added such vertices in order to partition a face. i.e. the kernel is not aware
                    // that a given input mesh it is working with is modified by the frontend (it assumes that the meshes is exactly as was
                    // provided by the client).
                    // So, here we have to fix that mapping information to correctly state that "any vertex added due to face
                    // partitioning was not in the user provided input mesh" and should therefore be treated/labelled as an intersection
                    // point i.e. it should map to UINT32_MAX because it does not map to any vertex in the client-provided input mesh.
                    bool vertex_exists_due_to_face_partitioning = false;
                    // this flag tells us whether the current vertex maps to one in the internal version of the source mesh
                    // i.e. it does not map to the internal version cut-mesh
                    const bool internal_input_mesh_vertex_is_for_source_mesh = (internal_input_mesh_vertex_idx < cc_uptr->internal_sourcemesh_vertex_count);

                    if (internal_input_mesh_vertex_is_for_source_mesh) {
                        const std::unordered_map<vd_t, vec3>::const_iterator fiter = cc_uptr->source_hmesh_new_poly_partition_vertices->find(vd_t(internal_input_mesh_vertex_idx));
                        vertex_exists_due_to_face_partitioning = (fiter != cc_uptr->source_hmesh_new_poly_partition_vertices->cend());
                    } else // i.e. internal_input_mesh_vertex_is_for_cut_mesh
                    {
                        std::unordered_map<vd_t, vec3>::const_iterator fiter = cc_uptr->cut_hmesh_new_poly_partition_vertices->find(vd_t(internal_input_mesh_vertex_idx));
                        vertex_exists_due_to_face_partitioning = (fiter != cc_uptr->cut_hmesh_new_poly_partition_vertices->cend());
                    }

                    if (!vertex_exists_due_to_face_partitioning) { // i.e. is a client-mesh vertex (an original vertex)

                        MCUT_ASSERT(cc_uptr->internal_sourcemesh_vertex_count > 0);

                        if (!internal_input_mesh_vertex_is_for_source_mesh) // is it a cut-mesh vertex discriptor ..?
                        {
                            // vertices added due to face-partitioning will have an offsetted index/descriptor that is >= client_sourcemesh_vertex_count
                            const uint32_t internal_input_mesh_vertex_idx_without_offset = (internal_input_mesh_vertex_idx - cc_uptr->internal_sourcemesh_vertex_count);
                            client_input_mesh_vertex_idx = (internal_input_mesh_vertex_idx_without_offset + cc_uptr->client_sourcemesh_vertex_count); // ensure that we offset using number of [user-provided mesh] vertices
                        } else {
                            client_input_mesh_vertex_idx = internal_input_mesh_vertex_idx; // src-mesh vertices have no offset unlike cut-mesh vertices
                        }
                    }
                }

                *(casted_ptr + elem_offset) = client_input_mesh_vertex_idx;
                elem_offset++;
            }

            MCUT_ASSERT(elem_offset <= vertex_map_size);
        }
    } break;
    case MC_CONNECTED_COMPONENT_DATA_FACE_MAP: {

        const uint32_t face_map_size = cc_uptr->kernel_hmesh_data.data_maps.face_map.size();

        if (face_map_size == 0) {
            throw std::invalid_argument("face map not available"); // user probably forgot to set the dispatch flag
        }

        MCUT_ASSERT(face_map_size == (uint32_t)cc_uptr->kernel_hmesh_data.mesh.number_of_faces());

        if (pMem == nullptr) {
            *pNumBytes = face_map_size * sizeof(uint32_t); // each face has a map value (intersection point == uint_max)
        } else {
            if (bytes > (face_map_size * sizeof(uint32_t))) {
                throw std::invalid_argument("out of bounds memory access");
            }

            if ((bytes % sizeof(uint32_t)) != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }

            const uint32_t elems_to_copy = (bytes / sizeof(uint32_t));
            uint32_t elem_offset = 0;
            uint32_t* casted_ptr = reinterpret_cast<uint32_t*>(pMem);

            // TODO: make parallel
            for (uint32_t i = 0; i < elems_to_copy; ++i) // ... for each vertex (to copy) in CC
            {
                uint32_t internal_inputmesh_face_idx = (uint32_t)cc_uptr->kernel_hmesh_data.data_maps.face_map[i];
                uint32_t client_input_mesh_face_idx = INT32_MAX;
                const bool internal_input_mesh_face_idx_is_for_src_mesh = (internal_inputmesh_face_idx < cc_uptr->internal_sourcemesh_face_count);

                if (internal_input_mesh_face_idx_is_for_src_mesh) {

                    std::unordered_map<fd_t, fd_t>::const_iterator fiter = cc_uptr->source_hmesh_child_to_usermesh_birth_face->find(fd_t(internal_inputmesh_face_idx));

                    if (fiter != cc_uptr->source_hmesh_child_to_usermesh_birth_face->cend()) {
                        client_input_mesh_face_idx = fiter->second;
                    } else {
                        client_input_mesh_face_idx = internal_inputmesh_face_idx;
                    }
                    MCUT_ASSERT(client_input_mesh_face_idx < cc_uptr->client_sourcemesh_face_count);
                } else // internalInputMeshVertexDescrIsForCutMesh
                {
                    std::unordered_map<fd_t, fd_t>::const_iterator fiter = cc_uptr->cut_hmesh_child_to_usermesh_birth_face->find(fd_t(internal_inputmesh_face_idx));

                    if (fiter != cc_uptr->cut_hmesh_child_to_usermesh_birth_face->cend()) {
                        uint32_t unoffsettedDescr = (fiter->second - cc_uptr->internal_sourcemesh_face_count);
                        client_input_mesh_face_idx = unoffsettedDescr + cc_uptr->client_sourcemesh_face_count;
                    } else {
                        uint32_t unoffsettedDescr = (internal_inputmesh_face_idx - cc_uptr->internal_sourcemesh_face_count);
                        client_input_mesh_face_idx = unoffsettedDescr + cc_uptr->client_sourcemesh_face_count;
                    }
                }

                MCUT_ASSERT(client_input_mesh_face_idx != INT32_MAX);

                *(casted_ptr + elem_offset) = client_input_mesh_face_idx;
                elem_offset++;
            }

            MCUT_ASSERT(elem_offset <= face_map_size);
        }
    } break;
    case MC_CONNECTED_COMPONENT_DATA_FACE_TRIANGULATION: {

        if (cc_uptr->constrained_delaunay_triangulation_indices.empty()) // compute triangulation if not yet available
        {
            // internal halfedge data structure from the current connected component
            const hmesh_t& cc = cc_uptr->kernel_hmesh_data.mesh;

            uint32_t face_indices_offset = 0;
            cc_uptr->constrained_delaunay_triangulation_indices.reserve(cc.number_of_faces());

            // -----
            std::vector<vec3> cc_face_vcoords3d;
            // NOTE: the elements of this array might be reversed, which occurs
            // when the winding-order/orientation of "cc_face_iter" is flipped
            // due to projection (see call to project_to_2d())
            std::vector<vec2> cc_face_vcoords2d; 

            // edge of face, which are used by triangulator as "fixed edges" to
            // constrain the CDT
            std::vector<cdt::edge_t> cc_face_edges;
            //  list of indices which define all triangles that result from the CDT
            std::vector<uint32_t> cc_face_triangulation;
            // used to check that all indices where used in the triangulation.
            // If any entry is false after finshing triangulation then there will be a hole in the output
            // This is use for sanity checking
            std::vector<bool> cc_face_vtx_to_is_used_flag;
            // descriptors of vertices in face (they index into the CC)
            std::vector<vertex_descriptor_t> cc_face_vertices;

            // for each face (TODO: make parallel)
            for (face_array_iterator_t cc_face_iter = cc.faces_begin(); cc_face_iter != cc.faces_end(); ++cc_face_iter) {

                cc.get_vertices_around_face(cc_face_vertices, *cc_face_iter);

                // number of vertices of triangulated face
                const uint32_t cc_face_vcount = (uint32_t)cc_face_vertices.size();

                MCUT_ASSERT(cc_face_vcount >= 3);

                const bool cc_face_is_triangle = (cc_face_vcount == 3);

                if (cc_face_is_triangle) {

                    // for each vertex in face
                    for (uint32_t i = 0; i < cc_face_vcount; ++i) {
                        const uint32_t vertex_id_in_cc = (uint32_t)SAFE_ACCESS(cc_face_vertices, i);

                        cc_uptr->constrained_delaunay_triangulation_indices.push_back(vertex_id_in_cc);
                    }

                } else {

                    //
                    // need to triangulate face
                    //

                    //
                    // init vars (which we do not want to be re-inititalizing)
                    //
                    cc_face_vcoords3d.resize(cc_face_vcount);
                    cc_face_vcoords2d.clear(); // resized by project_to_2d(...)
                    cc_face_edges.clear();
                    cc_face_triangulation.clear();
                    cc_face_vtx_to_is_used_flag.resize(cc_face_vcount);

                    // for each vertex in face: get its coordinates
                    for (uint32_t i = 0; i < cc_face_vcount; ++i) {

                        const vertex_descriptor_t cc_face_vertex_descr = SAFE_ACCESS(cc_face_vertices, i);

                        const vec3& coords = cc.vertex(cc_face_vertex_descr);

                        SAFE_ACCESS(cc_face_vcoords3d, i) = coords;
                    }

                    // Project face-vertex coordinates to 2D
                    //
                    // NOTE: Although we are projecting using the plane normal of
                    // the plane, the shape and thus area of the face polygon is
                    // unchanged (but the winding order might change!). 
                    // See definition of "project_to_2d()"
                    // =====================================================

                    // Maps each vertex in face to the reversed index if the polygon
                    // winding order was reversed due to projection to 2D. Otherwise,
                    // Simply stores the indices from 0 to N-1
                    std::vector<uint32_t> face_to_cdt_vmap(cc_face_vcount); 
                    std::iota (std::begin(face_to_cdt_vmap), std::end(face_to_cdt_vmap), 0);

                    {
                        vec3 cc_face_normal_vector;
                        double cc_face_plane_eq_dparam; //
                        const int largest_component_of_normal = compute_polygon_plane_coefficients(
                            cc_face_normal_vector,
                            cc_face_plane_eq_dparam,
                            cc_face_vcoords3d.data(),
                            (int)cc_face_vcount);

                        project_to_2d(cc_face_vcoords2d, cc_face_vcoords3d, cc_face_normal_vector, largest_component_of_normal);

                        //
                        // determine the signed area to check if the 2D face polygon
                        // is CW (negative) or CCW (positive)
                        //

                        double signed_area = 0;

                        for(uint32_t i = 0; i < cc_face_vcount-2; ++i)
                        {
                            vec2 cur = cc_face_vcoords2d[i];
                            vec2 nxt = cc_face_vcoords2d[(i+1) % cc_face_vcount];
                            vec2 nxtnxt = cc_face_vcoords2d[(i+2) % cc_face_vcount];
                            signed_area += orient2d(cur, nxt, nxtnxt);
                        }

                        const bool winding_order_flipped_due_to_projection = (signed_area < 0);
                        
                        if(winding_order_flipped_due_to_projection)
                        {
                            // Reverse the order of points so that they are CCW
                            std::reverse(cc_face_vcoords2d.begin(), cc_face_vcoords2d.end());

                            // for each vertex index in face
                            for(int32_t i =0; i < (int32_t)cc_face_vcount; ++i)
                            {
                                // save reverse index map
                                face_to_cdt_vmap[i] = wrap_integer(-(i+1), 0, cc_face_vcount-1 );
                            }
                        }
                    }

                    // Winding order tracker (WOT):
                    // We use this halfedge data structure to ensure that the winding-order
                    // that is computed by the CDT triangulator is consistent with that
                    // of "cc_face_iter".
                    // Before triangulation, we populate it with the vertices, (half)edges and
                    // faces of the neighbours of "cc_face_iter". This information we will be
                    // used to check for proper winding-order when we later insert the CDT
                    // triangles whose winding order we assume to be inconsistent with
                    // "cc_face_iter"
                    hmesh_t wot;
                    // vertex descriptor map (from WOT to CC)
                    // std::map<vertex_descriptor_t, vertex_descriptor_t> wot_to_cc_vmap;

                    // vertex descriptor map (from CC to WOT)
                    std::map<vertex_descriptor_t, vertex_descriptor_t> cc_to_wot_vmap;

                    // The halfedge with-which we will identify the first CDT triangle to insert into the
                    // array "cc_face_triangulation" (see below when we actually do insertion).
                    //
                    // The order of triangle insertion must priotise the triangle adjacent to the boundary,
                    // which are those that are incident to a fixed-edge in the CDT triangulators output.
                    // We need "cc_seed_halfedge" to ensure that the first CDT triangle to be inserted is inserted with the
                    // correct winding order. This caters to the scenario where "WOT" does not
                    // contain enough information to be able to reject the winding-order with which we
                    // attempt to insert _the first_ CDT triangle into "cc_face_triangulation".
                    //
                    // It is perfectly possible for "cc_seed_halfedge" to remain null, which will happen if "cc_face_iter"
                    // is the only face in the connected component.
                    halfedge_descriptor_t cc_seed_halfedge = hmesh_t::null_halfedge();
                    // ... those we have already saved in the wot
                    // This is needed to prevent attempting to add the same neighbour face into
                    // the WOT, which can happen if the cc_face_iter shares two or more edges
                    // with a neighbours (this is possible since our connected components
                    // can have n-gon faces )
                    std::unordered_set<face_descriptor_t> wot_traversed_neighbours;
                    // ... in CCW order
                    const std::vector<halfedge_descriptor_t>& cc_face_halfedges = cc.get_halfedges_around_face(*cc_face_iter);

                    // for each halfedge of face
                    for (std::vector<halfedge_descriptor_t>::const_iterator hiter = cc_face_halfedges.begin(); hiter != cc_face_halfedges.end(); ++hiter) {

                        halfedge_descriptor_t h = *hiter;
                        halfedge_descriptor_t opph = cc.opposite(h);
                        face_descriptor_t neigh = cc.face(opph);

                        const bool neighbour_exists = (neigh != hmesh_t::null_face());

                        // neighbour exists and we have not already traversed it
                        // by adding it into the WOT
                        if (neighbour_exists && wot_traversed_neighbours.count(neigh) == 0) {

                            if (cc_seed_halfedge == hmesh_t::null_halfedge()) {
                                cc_seed_halfedge = h; // set once based on first neighbour
                            }

                            //
                            // insert the neighbour into WOT.
                            // REMEMBER: the stored connectivity information is what we
                            // will use to ensure that we insert triangles into "cc_face_triangulation"
                            // with the correct orientation.
                            //

                            const std::vector<vertex_descriptor_t>& vertices_around_neighbour = cc.get_vertices_around_face(neigh);

                            // face vertices (their descriptors for indexing into the WOT)
                            std::vector<vertex_descriptor_t> remapped_descrs; // from CC to WOT

                            // for each vertex around neighbour
                            for (std::vector<vertex_descriptor_t>::const_iterator neigh_viter = vertices_around_neighbour.cbegin();
                                 neigh_viter != vertices_around_neighbour.cend(); ++neigh_viter) {

                                // Check if vertex is already added into the WOT
                                std::map<vertex_descriptor_t, vertex_descriptor_t>::const_iterator cc_to_wot_vmap_iter = cc_to_wot_vmap.find(*neigh_viter);

                                if (cc_to_wot_vmap_iter == cc_to_wot_vmap.cend()) { // if not ..

                                    const vec3& neigh_vertex_coords = cc.vertex(*neigh_viter);

                                    const vertex_descriptor_t woe_vdescr = wot.add_vertex(neigh_vertex_coords);

                                    cc_to_wot_vmap_iter = cc_to_wot_vmap.insert(std::make_pair(*neigh_viter, woe_vdescr)).first;
                                }

                                MCUT_ASSERT(cc_to_wot_vmap_iter != cc_to_wot_vmap.cend());

                                remapped_descrs.push_back(cc_to_wot_vmap_iter->second);
                            }

                            // add the neighbour into WOT
                            face_descriptor_t nfd = wot.add_face(remapped_descrs);

                            MCUT_ASSERT(nfd != hmesh_t::null_face());
                        }

                        wot_traversed_neighbours.insert(neigh);
                    }

                    // Add (remaining) vertices of "cc_face_iter" into WOT.
                    //
                    // NOTE: some (or all) of the vertices of the "cc_face_iter"
                    // might already have been added when registering the neighbours.
                    // However, we must still check that all vertices have been added
                    // since a vertex is added (during the previous neighbour
                    // registration phase) if-and-only-if it is used by a neighbour.
                    // Thus vertices are only added during neighbour registration
                    // phase if they are incident to an edge that is shared with another
                    // face.
                    // If "cc_face_iter" has zero neighbours then non of it vertices
                    // will have been added in the previous phase.
                    // =======================================

                    // vertex descriptor map (from CDT to WOT)
                    std::map<uint32_t, vertex_descriptor_t> cdt_to_wot_vmap;
                    // vertex descriptor map (from WOT to CDT)
                    std::map<vertex_descriptor_t, uint32_t> wot_to_cdt_vmap;

                    // for each vertex of face
                    for (uint32_t i = 0; i < cc_face_vcount; ++i) {

                        const vertex_descriptor_t cc_face_vertex_descr = SAFE_ACCESS(cc_face_vertices, i);

                        // check if vertex has already been added into the WOT
                        std::map<vertex_descriptor_t, vertex_descriptor_t>::const_iterator fiter = cc_to_wot_vmap.find(cc_face_vertex_descr);

                        if (fiter == cc_to_wot_vmap.cend()) { // ... if not

                            const vec3& coords = SAFE_ACCESS(cc_face_vcoords3d, i);

                            vertex_descriptor_t vd = wot.add_vertex(coords);

                            fiter = cc_to_wot_vmap.insert(std::make_pair(cc_face_vertex_descr, vd)).first;
                        }

                        cdt_to_wot_vmap[i] = fiter->second;
                        wot_to_cdt_vmap[fiter->second] = i;
                    }

                    //
                    // In the following section, we will check-for and handle
                    // the case of having duplicate vertices in "cc_face_iter".
                    //
                    // Duplicate vertices arise when "cc_face_iter" is from the source-mesh 
                    // and it has a partial-cut. Example: source-mesh=triangle and 
                    // cut-mesh=triangle, where the cut-mesh does not split the source-mesh into
                    // two disjoint parts (i.e. a triangle and a quad) but instead
                    // induces a slit
                    //

                    // Find the duplicates (if any)
                    const cdt::duplicates_info_t duplicates_info_pre = cdt::find_duplicates<double>(
                        cc_face_vcoords2d.begin(),
                        cc_face_vcoords2d.end(),
                        cdt::get_x_coord_vec2d<double>,
                        cdt::get_y_coord_vec2d<double>);
                    
                    // number of duplicate vertices (if any)
                    const uint32_t duplicate_vcount = (uint32_t)duplicates_info_pre.duplicates.size();
                    const bool have_duplicates = duplicate_vcount > 0;

                    if (have_duplicates) {

                        // for each pair of duplicate vertices
                        for (std::vector<std::size_t>::const_iterator duplicate_vpair_iter = duplicates_info_pre.duplicates.cbegin();
                             duplicate_vpair_iter != duplicates_info_pre.duplicates.cend(); ++duplicate_vpair_iter) {

                            //
                            // The two vertices are duplicates because they have the _exact_ same coordinates.
                            // We make these points unique by perturbing the coordinates of one of them. This requires care
                            // because we want to ensure that "cc_face_iter" remains a simple polygon (without
                            // self-intersections) after perturbation. To do this, we must perturbation one
                            // vertex in the direction that lies on the left-side (i.e. CCW dir) of the two
                            // halfedges incident to that vertex. We also take care to account for the fact the two
                            // incident edges may be parallel.
                            //

                                // current duplicate vertex (index in "cc_face_iter")
                                const std::int32_t perturbed_dvertex_id = (std::uint32_t)(*duplicate_vpair_iter);
                                // previous vertex (in "cc_face_iter") from current duplicate vertex
                                const std::uint32_t prev_vtx_id = wrap_integer(perturbed_dvertex_id - 1, 0, cc_face_vcount - 1);
                                // next vertex (in "cc_face_iter") from current duplicate vertex
                                const std::uint32_t next_vtx_id = wrap_integer(perturbed_dvertex_id + 1, 0, cc_face_vcount - 1);
                                // the other duplicate vertex of pair
                                const std::int32_t other_dvertex_id = (std::uint32_t)SAFE_ACCESS(duplicates_info_pre.mapping, perturbed_dvertex_id);

                                vec2& perturbed_dvertex_coords = SAFE_ACCESS(cc_face_vcoords2d, perturbed_dvertex_id); // will be modified by shifting/perturbation
                                const vec2& prev_vtx_coords = SAFE_ACCESS(cc_face_vcoords2d, prev_vtx_id);
                                const vec2& next_vtx_coords = SAFE_ACCESS(cc_face_vcoords2d, next_vtx_id);

                                // vector along incident edge, pointing from current to previous vertex (NOTE: clockwise dir, reverse)
                                const vec2 to_prev = prev_vtx_coords - perturbed_dvertex_coords;
                                // vector along incident edge, pointing from current to next vertex (NOTE: counter-clockwise dir, normal)
                                const vec2 to_next = next_vtx_coords - perturbed_dvertex_coords;

                                // positive-value if three points are in CCW order (sign_t::ON_POSITIVE_SIDE)
                                // negative-value if three points are in CW order (sign_t::ON_NEGATIVE_SIDE)
                                // zero if collinear (sign_t::ON_ORIENTED_BOUNDARY)
                                const double orient2d_res = orient2d(perturbed_dvertex_coords, next_vtx_coords, prev_vtx_coords);
                                const sign_t orient2d_sgn = sign(orient2d_res);

                                const double to_prev_sqr_len = squared_length(to_prev);
                                const double to_next_sqr_len = squared_length(to_next);

                                //
                                // Now we must determine which side is the perturbation_vector must be
                                // pointing. i.e. the side of "perturbed_dvertex_coords" or the side 
                                // of its duplicate
                                //
                                // NOTE: this is only really necessary if the partially cut polygon
                                // Has more that 3 intersection points (i.e. more than the case of 
                                // one tip, and two duplicates)
                                //

                                const int32_t flip = (orient2d_sgn == sign_t::ON_NEGATIVE_SIDE) ? -1 : 1;
                                
                                //
                                // Compute the perturbation vector as the average of the two incident edges eminating
                                // from the current vertex. NOTE: This perturbation vector should generally point in
                                // the direction of the polygon-interior (i.e. analogous to pushing the polygon at
                                // the location represented by perturbed_dvertex_coords) to cause a minute dent due to small
                                // loss of area.
                                // Normalization happens below
                                vec2 perturbation_vector = ((to_prev + to_next) / 2.0) * flip;

                                // "orient2d()" is exact in the sense that it can depend on computations with numbers
                                // whose magnitude is lower than the threshold "orient2d_ccwerrboundA". It follows
                                // that this threshold is too "small" a number for us to be able to reliably compute
                                // stuff with the result of "orient2d()" that is near this threshold.
                                const double errbound = 1e-2;

                                // We use "errbound", rather than "orient2d_res", to determine if the incident edges
                                // are parallel to give us sufficient room of numerical-precision to reliably compute
                                // the perturbation vector.
                                // In general, if the incident edges are not parallel then the perturbation vector
                                // is computed as the mean of "to_prev" and "to_next". Thus, being "too close"
                                // (within some threshold) to the edges being parallel, can induce unpredicatable
                                // numerical instabilities, where the mean-vector will be too close to the zero-vector
                                // and can complicate the task of perturbation.
                                const bool incident_edges_are_parallel = std::fabs(orient2d_res) <= std::fabs(errbound);

                                if (incident_edges_are_parallel) {
                                    //
                                    // pick the shortest of the two incident edges and compute the
                                    // orthogonal perturbation vector as the counter-clockwise rotation
                                    // of this shortest incident edge.
                                    //

                                    // flip sign so that the edge is in the CCW dir by pointing from "prev" to "cur"
                                    vec2 edge_vec(-to_prev.x(), -to_prev.y());

                                    if (to_prev_sqr_len > to_next_sqr_len) {
                                        edge_vec = to_next; // pick shortest (NOTE: "to_next" is already in CCW dir)
                                    }

                                    // rotate the selected edge by 90 degrees
                                    const vec2 edge_vec_rotated90(-edge_vec.y(), edge_vec.x());

                                    perturbation_vector = edge_vec_rotated90;
                                }

                                const vec2 perturbation_dir = normalize(perturbation_vector);

                                //
                                // Compute the maximum length between any two vertices in "cc_face_iter" as the
                                // largest length between any two vertices.
                                //
                                // This will be used to scale "perturbation_dir" so that we find the
                                // closest edge (from "perturbed_dvertex_coords") that is intersected by this ray.
                                // We will use the resulting information to determine the amount by-which
                                // "perturbed_dvertex_coords" is to be perturbed.
                                //

                                // largest squared length between any two vertices in "cc_face_iter"
                                double largest_sqrd_length = -1.0;

                                for (uint32_t i = 0; i < cc_face_vcount; ++i) {

                                    const vec2& a = SAFE_ACCESS(cc_face_vcoords2d, i);

                                    for (uint32_t j = 0; j < cc_face_vcount; ++j) {

                                        if (i == j) {
                                            continue; // skip -> comparison is redundant
                                        }

                                        const vec2& b = SAFE_ACCESS(cc_face_vcoords2d, j);

                                        const double sqrd_length = squared_length(b - a);
                                        largest_sqrd_length = std::max(sqrd_length, largest_sqrd_length);
                                    }
                                }

                                //
                                // construct the segment with-which will will find the closest
                                // intersection point from "perturbed_dvertex_coords" to "perturbed_dvertex_coords + perturbation_dir*std::sqrt(largest_sqrd_length)"";
                                //

                                const double shift_len = std::sqrt(largest_sqrd_length);
                                const vec2 shift = perturbation_dir * shift_len;

                                vec2 intersection_point_on_edge = perturbed_dvertex_coords + shift; // some location potentially outside of polygon

                                {
                                    struct {
                                        vec2 start;
                                        vec2 end;
                                    } segment;
                                    segment.start = perturbed_dvertex_coords;
                                    segment.end = perturbed_dvertex_coords + shift;

                                    // test segment against all edges to find closest intersection point

                                    double segment_min_tval = 1.0;

                                    // for each edge of face to be triangulated (number of vertices == number of edges)
                                    for (std::uint32_t i = 0; i < cc_face_vcount; ++i) {
                                        const std::uint32_t edge_start_idx = i;
                                        const std::uint32_t edge_end_idx = (i + 1) % cc_face_vcount;

                                        if ((edge_start_idx == (uint32_t)perturbed_dvertex_id || edge_end_idx == (uint32_t)perturbed_dvertex_id) || //
                                            (edge_start_idx == (uint32_t)other_dvertex_id || edge_end_idx == (uint32_t)other_dvertex_id)) {
                                            continue; // impossible to properly intersect incident edges
                                        }

                                        const vec2& edge_start_coords = SAFE_ACCESS(cc_face_vcoords2d, edge_start_idx);
                                        const vec2& edge_end_coords = SAFE_ACCESS(cc_face_vcoords2d, edge_end_idx);

                                        double segment_tval; // parameter along segment
                                        double edge_tval; // parameter along current edge
                                        vec2 ipoint; // intersection point between segment and current edge

                                        const char result = compute_segment_intersection(
                                            segment.start, segment.end, edge_start_coords, edge_end_coords,
                                            ipoint, segment_tval, edge_tval);

                                        if (result == '1' && segment_min_tval > segment_tval) { // we have an clear intersection point
                                            segment_min_tval = segment_tval;
                                            intersection_point_on_edge = ipoint;
                                        } else if (
                                            // segment and edge are collinear
                                            result == 'e' ||
                                            // segment and edge are collinear, or one entity cuts through the vertex of the other
                                            result == 'v') {
                                            // pick the closest vertex of edge and compute "segment_tval" as a ratio of vector length

                                            // length from segment start to the start of edge
                                            const double sqr_dist_to_edge_start = squared_length(edge_start_coords - segment.start);
                                            // length from segment start to the end of edge
                                            const double sqr_dist_to_edge_end = squared_length(edge_end_coords - segment.start);

                                            // length from start of segment to either start of edge or end of edge (depending on which is closer)
                                            double sqr_dist_to_closest = sqr_dist_to_edge_start;
                                            const vec2* ipoint_ptr = &edge_start_coords;

                                            if (sqr_dist_to_edge_start > sqr_dist_to_edge_end) {
                                                sqr_dist_to_closest = sqr_dist_to_edge_end;
                                                ipoint_ptr = &edge_end_coords;
                                            }

                                            // ratio along segment
                                            segment_tval = std::sqrt(sqr_dist_to_closest) / shift_len;

                                            if (segment_min_tval > segment_tval) {
                                                segment_min_tval = segment_tval;
                                                intersection_point_on_edge = *ipoint_ptr; // closest point
                                            }
                                        }
                                    }

                                    MCUT_ASSERT(segment_min_tval <= 1.0); // ... because we started from max length between any two vertices
                                }

                                // Shortened perturbation vector: shortening from the vector that is as long as the
                                // max length between any two vertices in "cc_face_iter", to a vector that runs
                                // from "perturbed_dvertex_coords" and upto the boundary-point of the "cc_face_iter", along
                                // "perturbation_vector" and passing through the interior of "cc_face_iter")
                                const vec2 revised_perturbation_vector = (intersection_point_on_edge - perturbed_dvertex_coords);
                                const double revised_perturbation_len = length(revised_perturbation_vector);

                                const double scale = (errbound * revised_perturbation_len);
                                // The translation by which we perturb "perturbed_dvertex_coords"
                                //
                                // NOTE: since "perturbation_vector" was constructed from "to_prev" and "to_next",
                                // "displacement" is by-default pointing in the positive/CCW direction, which is torward
                                // the interior of the polygon represented by "cc_face_iter".
                                // Thus, the cases with "orient2d_sgn == sign_t::ON_POSITIVE_SIDE" and
                                // "orient2d_sgn == sign_t::ON_ORIENTED_BOUNDARY", result in the same displacement vector
                                const vec2 displacement = (perturbation_dir * scale);

                                // perturb
                                perturbed_dvertex_coords = perturbed_dvertex_coords + displacement;

                            //} // for (std::uint32_t dv_iter = 0; dv_iter < 2; ++dv_iter) {
                        } // for (std::vector<std::size_t>::const_iterator duplicate_vpair_iter = duplicates_info_pre.duplicates.cbegin(); ...
                    } // if (have_duplicates) {

                    //
                    // create the constraint edges for the CDT triangulator, which are just the edges of "cc_face_iter"
                    //
                    for (uint32_t i = 0; i < cc_face_vcount; ++i) {
                        cc_face_edges.push_back(cdt::edge_t(i, (i + 1) % cc_face_vcount));
                    }

                    // check for duplicate vertices again
                    const cdt::duplicates_info_t duplicates_info_post = cdt::find_duplicates<double>(
                        cc_face_vcoords2d.begin(),
                        cc_face_vcoords2d.end(),
                        cdt::get_x_coord_vec2d<double>,
                        cdt::get_y_coord_vec2d<double>);

                    if (!duplicates_info_post.duplicates.empty()) {
                        // This should not happen! Probably a good idea to email the author
                        context_uptr->log(
                            MC_DEBUG_SOURCE_KERNEL,
                            MC_DEBUG_TYPE_ERROR, 0,
                            MC_DEBUG_SEVERITY_HIGH, "face f" + std::to_string(*cc_face_iter) + " has duplicate vertices that could not be resolved (bug)");
                        continue; // skip to next face (will leave a hole in the output)
                    }

                    // allocate triangulator
                    cdt::triangulator_t<double> cdt(cdt::vertex_insertion_order_t::AS_GIVEN);
                    cdt.insert_vertices(cc_face_vcoords2d); // potentially perturbed (if duplicates exist)
                    cdt.insert_edges(cc_face_edges);
                    cdt.erase_outer_triangles(); // do the constrained delaunay triangulation

                    // const std::unordered_map<cdt::edge_t, std::vector<cdt::edge_t>> tmp = cdt::edge_to_pieces_mapping(cdt.pieceToOriginals);
                    // const std::unordered_map<cdt::edge_t, std::vector<std::uint32_t>> edgeToSplitVerts = cdt::get_edge_to_split_vertices_map(tmp, cdt.vertices);

                    if (!cdt::check_topology(cdt)) {

                        context_uptr->log(
                            MC_DEBUG_SOURCE_KERNEL,
                            MC_DEBUG_TYPE_OTHER, 0,
                            MC_DEBUG_SEVERITY_NOTIFICATION, "triangulation on face f" + std::to_string(*cc_face_iter) + " has invalid topology");

                        continue; // skip to next face (will leave a hole in the output)
                    }

                    if (cdt.triangles.empty()) {
                        context_uptr->log(
                            MC_DEBUG_SOURCE_KERNEL,
                            MC_DEBUG_TYPE_OTHER, 0,
                            MC_DEBUG_SEVERITY_NOTIFICATION, "triangulation on face f" + std::to_string(*cc_face_iter) + " produced zero faces");

                        continue; // skip to next face (will leave a hole in the output)
                    }

                    //
                    // In the following, we will now save the produce triangles into the
                    // output array "cc_face_triangulation".
                    //

                    // number of CDT triangles
                    const uint32_t cc_face_triangle_count = (uint32_t)cdt.triangles.size();

                    //
                    // We insert triangles into "cc_face_triangulation" by using a
                    // breadth-first search-like flood-fill strategy to "walk" the
                    // triangles of the CDT. We start from a prescribed triangle next to the
                    // boundary of "cc_face_iter".
                    //

                    // map vertices to CDT triangles
                    // Needed for the BFS traversal of triangles
                    std::vector<std::vector<uint32_t>> vertex_to_triangle_map(cc_face_vcount, std::vector<uint32_t>());

                    // for each CDT triangle
                    for (uint32_t i = 0; i < cc_face_triangle_count; ++i) {

                        const cdt::triangle_t& triangle = SAFE_ACCESS(cdt.triangles, i);

                        // for each triangle vertex
                        for (uint32_t j = 0; j < 3; j++) {
                            const uint32_t cdt_vertex_id = SAFE_ACCESS(triangle.vertices, j);
                            const uint32_t cc_face_vertex_id = SAFE_ACCESS(face_to_cdt_vmap, cdt_vertex_id);
                            std::vector<uint32_t>& incident_triangles = SAFE_ACCESS(vertex_to_triangle_map, cc_face_vertex_id);
                            incident_triangles.push_back(i); // save mapping
                        }
                    }

                    // start with any boundary edge (AKA constraint/fixed edge)
                    std::unordered_set<cdt::edge_t>::const_iterator fixed_edge_iter = cdt.fixedEdges.cbegin();

                    // NOTE: in the case that "cc_seed_halfedge" is null, then "cc_face_iter"
                    // is the only face in its connected component (mesh) and therefore
                    // it has no neighbours. In this case, the winding-order of the produced triangles
                    // is dependent on the CDT triangulator. The MCUT frontend will at best be able to
                    // ensure that all CDT triangles have consistent winding order (even if the triangulator
                    // produced mixed winding orders between the resulting triangles) but we cannot guarrantee
                    // the "front-facing" side of triangulated "cc_face_iter" will match that of its
                    // original non-triangulated form from the connected component.
                    //
                    // We leave that to the user to fix upon visual inspection.
                    //
                    const bool have_seed_halfedge = cc_seed_halfedge != hmesh_t::null_halfedge();

                    if (have_seed_halfedge) {
                        // if the seed halfedge exists then the triangulated face must have
                        // atleast one neighbour
                        MCUT_ASSERT(wot.number_of_faces() != 0);

                        // source and target descriptor in the connected component
                        const vertex_descriptor_t cc_seed_halfedge_src = cc.source(cc_seed_halfedge);
                        const vertex_descriptor_t cc_seed_halfedge_tgt = cc.target(cc_seed_halfedge);

                        // source and target descriptor in the face
                        const vertex_descriptor_t woe_src = SAFE_ACCESS(cc_to_wot_vmap, cc_seed_halfedge_src);
                        const uint32_t cdt_src = SAFE_ACCESS(wot_to_cdt_vmap, woe_src);
                        const vertex_descriptor_t woe_tgt = SAFE_ACCESS(cc_to_wot_vmap, cc_seed_halfedge_tgt);
                        const uint32_t cdt_tgt = SAFE_ACCESS(wot_to_cdt_vmap, woe_tgt);

                        // find the fixed edge in the CDT matching the vertices of the seed halfedge

                        fixed_edge_iter = std::find_if(
                            cdt.fixedEdges.cbegin(),
                            cdt.fixedEdges.cend(),
                            [&](const cdt::edge_t& e) -> bool {
                                return (e.v1() == cdt_src && e.v2() == cdt_tgt) || //
                                    (e.v2() == cdt_src && e.v1() == cdt_tgt);
                            });

                        MCUT_ASSERT(fixed_edge_iter != cdt.fixedEdges.cend());
                    }

                    // must always exist since cdt edge ultimately came from the CC, and also
                    // due to the fact that we have inserted edges into the CDT
                    MCUT_ASSERT(fixed_edge_iter != cdt.fixedEdges.cend());

                    // get the two vertices of the "seed" fixed edge (indices into CDT)
                    const std::uint32_t fixed_edge_vtx0_id = fixed_edge_iter->v1();
                    const std::uint32_t fixed_edge_vtx1_id = fixed_edge_iter->v2();

                    //
                    // Since these vertices share an edge, they will share a triangle in the CDT
                    // So lets get that shared triangle, which will be the seed triangle for the
                    // traversal process, which we will use to walk and insert triangles into
                    // the output array "cc_face_triangulation"
                    //

                    // incident triangles of first vertex
                    const std::vector<std::uint32_t>& fixed_edge_vtx0_tris = SAFE_ACCESS(vertex_to_triangle_map, fixed_edge_vtx0_id);
                    MCUT_ASSERT(fixed_edge_vtx0_tris.empty() == false);
                    // incident triangles of second vertex
                    const std::vector<std::uint32_t>& fixed_edge_vtx1_tris = SAFE_ACCESS(vertex_to_triangle_map, fixed_edge_vtx1_id);
                    MCUT_ASSERT(fixed_edge_vtx1_tris.empty() == false);

                    // the shared triangle between the two vertices of fixed edge
                    std::uint32_t fix_edge_seed_triangle = cdt::null_neighbour;

                    // for each CDT triangle incident to the first vertex
                    for (std::vector<std::uint32_t>::const_iterator it = fixed_edge_vtx0_tris.begin(); it != fixed_edge_vtx0_tris.end(); ++it) {

                        if (*it == cdt::null_neighbour) {
                            continue;
                        }

                        // does it exist in the incident triangle list of the other vertex?
                        if (std::find(fixed_edge_vtx1_tris.begin(), fixed_edge_vtx1_tris.end(), *it) != fixed_edge_vtx1_tris.end()) {
                            fix_edge_seed_triangle = *it; // found
                            break; // done
                        }
                    }

                    MCUT_ASSERT(fix_edge_seed_triangle != cdt::null_neighbour);

                    std::stack<std::uint32_t> seeds(std::deque<std::uint32_t>(1, fix_edge_seed_triangle));

                    // collection of traversed CDT triangles
                    std::unordered_set<std::uint32_t> traversed;

                    while (!seeds.empty()) { // while we still have triangles to walk

                        const std::uint32_t curr_triangle_id = seeds.top();
                        seeds.pop();

                        traversed.insert(curr_triangle_id); // those we have walked

                        const cdt::triangle_t& triangle = cdt.triangles[curr_triangle_id];

                        //
                        // insert current triangle into our triangulated CC mesh
                        //
                        const uint32_t triangle_vertex_count = 3;

                        // from CDT/"cc_face_iter" indices to WOT descriptors
                        std::vector<vertex_descriptor_t> remapped_triangle(triangle_vertex_count, hmesh_t::null_vertex());

                        // for each vertex of triangle
                        for (uint32_t i = 0; i < triangle_vertex_count; i++)
                        {
                            // index of current vertex in CDT/"cc_face_iter"
                            const uint32_t cdt_vertex_id = SAFE_ACCESS(triangle.vertices, i);
                            const uint32_t cc_face_vertex_id = SAFE_ACCESS(face_to_cdt_vmap, cdt_vertex_id);
                            
                            // mark vertex as used (for sanity check)
                            SAFE_ACCESS(cc_face_vtx_to_is_used_flag, cc_face_vertex_id) = true; 

                            // remap triangle vertex index
                            SAFE_ACCESS(remapped_triangle, i) = SAFE_ACCESS(cdt_to_wot_vmap, cc_face_vertex_id);

                            // save index into output array (where every three indices is a triangle)
                            cc_face_triangulation.emplace_back(cc_face_vertex_id);
                        }

                        // check that the winding order respects the winding order of "cc_face_iter"
                        const bool is_insertible = wot.is_insertable(remapped_triangle);

                        if (!is_insertible) { // CDT somehow produce a triangle with reversed winding-order (i.e. CW)

                            // flip the winding order by simply swapping indices
                            uint32_t a = remapped_triangle[0];
                            uint32_t c = remapped_triangle[2];
                            std::swap(a, c); // swap indices in the remapped triangle
                            remapped_triangle[0] = vertex_descriptor_t(a);
                            remapped_triangle[2] = vertex_descriptor_t(c);
                            const size_t N = cc_face_triangulation.size();

                            // swap indice in the saved triangle
                            std::swap(cc_face_triangulation[N - 1], cc_face_triangulation[N - 3]); // reverse last added triangle's indices
                        }

                        // add face into our WO wot
                        face_descriptor_t fd = wot.add_face(remapped_triangle); // keep track of added triangles from CDT

                        // if this happens then CDT gave us a strange triangulation e.g. duplicate triangles with opposite winding order
                        if (fd == hmesh_t::null_face()) {
                            // Simply remove/ignore the offending triangle. We cannot do anything at this stage.
                            cc_face_triangulation.pop_back();
                            cc_face_triangulation.pop_back();
                            cc_face_triangulation.pop_back();

                            const std::string msg = "triangulation on face f" + std::to_string(*cc_face_iter) + " produced invalid triangles that could not be stored";

                            context_uptr->log(
                                MC_DEBUG_SOURCE_KERNEL,
                                MC_DEBUG_TYPE_OTHER, 0,
                                MC_DEBUG_SEVERITY_HIGH, msg);
                        }

                        //
                        // We will now add the neighbouring CDT triangles into queue/stack
                        //

                        // for each CDT vertex
                        for (std::uint32_t i(0); i < triangle_vertex_count; ++i) {
                            
                            const uint32_t next = triangle.vertices[cdt::ccw(i)];
                            const uint32_t prev = triangle.vertices[cdt::cw(i)];
                            const cdt::edge_t query_edge(next, prev);
                            
                            if (cdt.fixedEdges.count(query_edge)) {
                                continue; // current edge is fixed edge so there is no neighbour
                            }

                            const std::uint32_t neighbour_index = triangle.neighbors[cdt::get_opposite_neighbour_from_vertex(i)];

                            if (neighbour_index != cdt::null_neighbour && traversed.count(neighbour_index) == 0) {
                                seeds.push(neighbour_index);
                            }
                        }
                    } // while (!seeds.empty()) {

                    // every triangle in the finalized CDT must be walked!
                    MCUT_ASSERT(traversed.size() == cdt.triangles.size()); // this might be violated if CDT produced duplicate triangles

                    //
                    // Final sanity check
                    //

                    for (std::uint32_t i = 0; i < (std::uint32_t)cc_face_vcount; ++i) {
                        if (SAFE_ACCESS(cc_face_vtx_to_is_used_flag, i) != true) {
                            context_uptr->log(
                                MC_DEBUG_SOURCE_KERNEL,
                                MC_DEBUG_TYPE_OTHER, 0,
                                MC_DEBUG_SEVERITY_HIGH, "triangulation on face f" + std::to_string(*cc_face_iter) + " did not use vertex v" + std::to_string(i));
                        }
                    }

                    //
                    // Change local triangle indices to global index values (in CC) and save
                    //

                    const uint32_t cc_face_triangulation_index_count = (uint32_t)cc_face_triangulation.size();
                    cc_uptr->constrained_delaunay_triangulation_indices.reserve(
                        cc_uptr->constrained_delaunay_triangulation_indices.size() + cc_face_triangulation_index_count
                    );

                    for (uint32_t i = 0; i < cc_face_triangulation_index_count; ++i) {
                        const uint32_t local_idx = cc_face_triangulation[i]; // id local within the current face that we are triangulating
                        const uint32_t global_idx = (uint32_t)cc_face_vertices[local_idx]; 

                        cc_uptr->constrained_delaunay_triangulation_indices.push_back(global_idx);
                    }
                } //  if (cc_face_vcount == 3)

                face_indices_offset += cc_face_vcount;
            }

            MCUT_ASSERT(cc_uptr->constrained_delaunay_triangulation_indices.size() >= 3);

        } // if(cc_uptr->indexArrayMesh.numTriangleIndices == 0)

        const uint32_t num_triangulation_indices = (uint32_t)cc_uptr->constrained_delaunay_triangulation_indices.size();

        if (pMem == nullptr) // client pointer is null (asking for size)
        {
            MCUT_ASSERT(num_triangulation_indices >= 3);
            *pNumBytes = num_triangulation_indices * sizeof(uint32_t); // each each vertex has a map value (intersection point == uint_max)
        } else {
            MCUT_ASSERT(num_triangulation_indices >= 3);

            if (bytes > num_triangulation_indices * sizeof(uint32_t)) {
                throw std::invalid_argument("out of bounds memory access");
            }

            if (bytes % (sizeof(uint32_t)) != 0 || (bytes / sizeof(uint32_t)) % 3 != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }

            memcpy(pMem, reinterpret_cast<void*>(cc_uptr->constrained_delaunay_triangulation_indices.data()), bytes);
        }
    } break;
    default:
        throw std::invalid_argument("invalid enum flag");
    }
}

void release_connected_components_impl(
    const McContext context,
    uint32_t numConnComps,
    const McConnectedComponent* pConnComps)
{
    std::map<McContext, std::unique_ptr<context_t>>::iterator context_entry_iter = g_contexts.find(context);

    if (context_entry_iter == g_contexts.end()) {
        throw std::invalid_argument("invalid context");
    }

    const std::unique_ptr<context_t>& context_uptr = context_entry_iter->second;

    if (numConnComps > (uint32_t)context_uptr->connected_components.size()) {
        throw std::invalid_argument("invalid connected component count");
    }

    bool freeAll = numConnComps == 0 && pConnComps == NULL;

    if (freeAll) {
        context_uptr->connected_components.clear();
    } else {
        for (int i = 0; i < (int)numConnComps; ++i) {
            McConnectedComponent connCompId = pConnComps[i];

            std::map<McConnectedComponent, std::unique_ptr<connected_component_t, void (*)(connected_component_t*)>>::const_iterator cc_entry_iter = context_uptr->connected_components.find(connCompId);

            if (cc_entry_iter == context_uptr->connected_components.cend()) {
                throw std::invalid_argument("invalid connected component id");
            }

            context_uptr->connected_components.erase(cc_entry_iter);
        }
    }
}

void release_context_impl(
    McContext context)
{
    std::map<McContext, std::unique_ptr<context_t>>::iterator context_entry_iter = g_contexts.find(context);

    if (context_entry_iter == g_contexts.end()) {
        throw std::invalid_argument("invalid context");
    }

    g_contexts.erase(context_entry_iter);
}
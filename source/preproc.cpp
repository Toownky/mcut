#include "mcut/internal/frontend.h"

#include "mcut/internal/bvh.h"
#include "mcut/internal/hmesh.h"
#include "mcut/internal/kernel.h"
#include "mcut/internal/math.h"
#include "mcut/internal/utils.h"

#include <numeric> // std::partial_sum
#include <queue>
#include <random> // for numerical perturbation

// If the inputs are found to not be in general position, then we perturb the
// cut-mesh by this constant (scaled by bbox diag times a random variable [0.1-1.0]).
const double GENERAL_POSITION_ENFORCMENT_CONSTANT = 1e-4;
const int MAX_PERTUBATION_ATTEMPTS = 1 << 3;

// this function converts an index array mesh (e.g. as recieved by the dispatch
// function) into a halfedge mesh representation for the kernel backend.
bool client_input_arrays_to_hmesh(
    std::unique_ptr<context_t>& context_uptr,
    hmesh_t& halfedgeMesh,
    double& bboxDiagonal,
    const void* pVertices,
    const uint32_t* pFaceIndices,
    const uint32_t* pFaceSizes,
    const uint32_t numVertices,
    const uint32_t numFaces,
    const vec3* perturbation = NULL)
{
    TIMESTACK_PUSH(__FUNCTION__);

    context_uptr->log(MC_DEBUG_SOURCE_API, MC_DEBUG_TYPE_OTHER, 0, MC_DEBUG_SEVERITY_NOTIFICATION, "construct halfedge mesh");

    // minor optimization
    halfedgeMesh.reserve_for_additional_elements(numVertices);

    TIMESTACK_PUSH("add vertices");

    // did the user provide vertex arrays of 32-bit floats...?
    if (context_uptr->dispatchFlags & MC_DISPATCH_VERTEX_ARRAY_FLOAT) {
        const float* vptr = reinterpret_cast<const float*>(pVertices);

        // for each input mesh-vertex
        for (uint32_t i = 0; i < numVertices; ++i) {
            const float& x = vptr[(i * 3) + 0];
            const float& y = vptr[(i * 3) + 1];
            const float& z = vptr[(i * 3) + 2];

            // insert our vertex into halfedge mesh
            vd_t vd = halfedgeMesh.add_vertex(
                double(x) + (perturbation != NULL ? (*perturbation).x() : double(0.)),
                double(y) + (perturbation != NULL ? (*perturbation).y() : double(0.)),
                double(z) + (perturbation != NULL ? (*perturbation).z() : double(0.)));

            MCUT_ASSERT(vd != hmesh_t::null_vertex() && (uint32_t)vd < numVertices);
        }
    }
    // did the user provide vertex arrays of 64-bit double...?
    else if (context_uptr->dispatchFlags & MC_DISPATCH_VERTEX_ARRAY_DOUBLE) {
        const double* vptr = reinterpret_cast<const double*>(pVertices);

        // for each input mesh-vertex
        for (uint32_t i = 0; i < numVertices; ++i) {
            const double& x = vptr[(i * 3) + 0];
            const double& y = vptr[(i * 3) + 1];
            const double& z = vptr[(i * 3) + 2];

            // insert our vertex into halfedge mesh
            vd_t vd = halfedgeMesh.add_vertex(
                double(x) + (perturbation != NULL ? (*perturbation).x() : double(0.)),
                double(y) + (perturbation != NULL ? (*perturbation).y() : double(0.)),
                double(z) + (perturbation != NULL ? (*perturbation).z() : double(0.)));

            MCUT_ASSERT(vd != hmesh_t::null_vertex() && (uint32_t)vd < numVertices);
        }
    }

    TIMESTACK_POP();

    // compute the mesh bounding box while we are at it (for numerical perturbation)
    vec3 bboxMin(1e10);
    vec3 bboxMax(-1e10);

    TIMESTACK_PUSH("create bbox");
    for (vertex_array_iterator_t i = halfedgeMesh.vertices_begin(); i != halfedgeMesh.vertices_end(); ++i) {
        const vec3& coords = halfedgeMesh.vertex(*i);
        bboxMin = compwise_min(bboxMin, coords);
        bboxMax = compwise_max(bboxMax, coords);
    }
    bboxDiagonal = length(bboxMax - bboxMin);
    TIMESTACK_POP();

    TIMESTACK_PUSH("create faces");

    const bool assume_triangle_mesh = (pFaceSizes == nullptr);

#if defined(MCUT_MULTI_THREADED)
    std::vector<uint32_t> partial_sums(numFaces, 0); // prefix sum result
    std::partial_sum(pFaceSizes, pFaceSizes + numFaces, partial_sums.data());

    {
        typedef std::vector<uint32_t>::const_iterator InputStorageIteratorType;
        typedef std::pair<InputStorageIteratorType, InputStorageIteratorType> OutputStorageType; // range of faces
        std::atomic_int atm_result;
        atm_result.store((int)McResult::MC_NO_ERROR); // 0 = ok;/ 1 = invalid face size; 2 invalid vertex index

        std::vector<std::vector<vd_t>> faces(numFaces);

        auto fn_create_faces = [&](
                                   InputStorageIteratorType block_start_,
                                   InputStorageIteratorType block_end_) -> OutputStorageType {
            for (InputStorageIteratorType i = block_start_; i != block_end_; ++i) {
                uint32_t faceID = (uint32_t)std::distance(partial_sums.cbegin(), i);
                std::vector<vd_t>& faceVertices = faces[faceID];
                int face_vertex_count = assume_triangle_mesh ? 3 : ((uint32_t*)pFaceSizes)[faceID];

                if (face_vertex_count < 3) {
                    int zero = (int)McResult::MC_NO_ERROR;
                    bool exchanged = atm_result.compare_exchange_strong(zero, 1);
                    if (exchanged) // first thread to detect error
                    {
                        context_uptr->log( //
                            MC_DEBUG_SOURCE_API, //
                            MC_DEBUG_TYPE_ERROR, //
                            0, //
                            MC_DEBUG_SEVERITY_HIGH, //
                            "invalid face-size for face - " + std::to_string(faceID) + " (size = " + std::to_string(face_vertex_count) + ")");
                    }
                    break;
                }

                faceVertices.resize(face_vertex_count);
                int faceBaseOffset = (*i) - face_vertex_count;

                for (int j = 0; j < face_vertex_count; ++j) {
                    uint32_t idx = ((uint32_t*)pFaceIndices)[faceBaseOffset + j];

                    MCUT_ASSERT(idx < numVertices);

                    const vertex_descriptor_t descr(idx);
                    const bool isDuplicate = std::find(faceVertices.cbegin(), faceVertices.cend(), descr) != faceVertices.cend();

                    if (isDuplicate) {
                        int zero = (int)McResult::MC_NO_ERROR;
                        bool exchanged = atm_result.compare_exchange_strong(zero, 2);

                        if (exchanged) // first thread to detect error
                        {
                            context_uptr->log(
                                MC_DEBUG_SOURCE_API,
                                MC_DEBUG_TYPE_ERROR,
                                0,
                                MC_DEBUG_SEVERITY_HIGH,
                                "found duplicate vertex in face - " + std::to_string(faceID));
                        }
                        break;
                    }

                    faceVertices[j] = (descr);
                }
            }
            return std::make_pair(block_start_, block_end_);
        };

        std::vector<std::future<OutputStorageType>> futures;
        OutputStorageType partial_res;

        parallel_fork_and_join(
            context_uptr->scheduler,
            partial_sums.cbegin(),
            partial_sums.cend(),
            (1 << 8),
            fn_create_faces,
            partial_res, // output computed by master thread
            futures);

        auto add_faces = [&](InputStorageIteratorType block_start_,
                             InputStorageIteratorType block_end_) -> bool {
            for (InputStorageIteratorType face_iter = block_start_;
                 face_iter != block_end_; ++face_iter) {
                uint32_t faceID = (uint32_t)std::distance(partial_sums.cbegin(), face_iter);
                const std::vector<vd_t>& faceVertices = SAFE_ACCESS(faces, faceID);
                fd_t fd = halfedgeMesh.add_face(faceVertices);

                if (fd == hmesh_t::null_face()) {

                    context_uptr->log( //
                        MC_DEBUG_SOURCE_API, //
                        MC_DEBUG_TYPE_ERROR, //
                        0, //
                        MC_DEBUG_SEVERITY_HIGH, //
                        "invalid vertices on face - " + std::to_string(faceID));
                    return false;
                }
            }
            return true;
        };

        bool okay = true;
        for (int i = 0; i < (int)futures.size(); ++i) {
            std::future<OutputStorageType>& f = futures[i];
            MCUT_ASSERT(f.valid()); // The behavior is undefined if valid()== false before the call to wait_for
            OutputStorageType future_res = f.get();

            const int val = atm_result.load();
            okay = okay && val == 0;
            if (!okay) {
                continue; // just go on (to next iteration) in order to at-least wait for all tasks to finish before we return to user
            }

            bool result = add_faces(future_res.first, future_res.second);
            okay = okay && result == true;
        }

        if (!okay) {
            return false;
        }

        // add lastly in order to maintain order
        bool result = add_faces(partial_res.first, partial_res.second);
        if (!result) {
            return false;
        }
    }
#else // #if defined(MCUT_MULTI_THREADED)
    int faceSizeOffset = 0;
    std::vector<vd_t> faceVertices;

    for (uint32_t i = 0; i < numFaces; ++i) {
        faceVertices.clear();
        int face_vertex_count =  assume_triangle_mesh ? 3 : ((uint32_t*)pFaceSizes)[i];

        if (face_vertex_count < 3) {

            context_uptr->log(MC_DEBUG_SOURCE_API, MC_DEBUG_TYPE_ERROR, 0, MC_DEBUG_SEVERITY_HIGH, "invalid face-size for face - " + std::to_string(i) + " (size = " + std::to_string(face_vertex_count) + ")");

            return false;
        }

        for (int j = 0; j < face_vertex_count; ++j) {

            uint32_t idx = ((uint32_t*)pFaceIndices)[faceSizeOffset + j];
            const vertex_descriptor_t descr(idx); // = fIter->second; //vmap[*fIter.first];
            const bool isDuplicate = std::find(faceVertices.cbegin(), faceVertices.cend(), descr) != faceVertices.cend();

            if (isDuplicate) {

                context_uptr->log(MC_DEBUG_SOURCE_API, MC_DEBUG_TYPE_ERROR, 0, MC_DEBUG_SEVERITY_HIGH, "found duplicate vertex in face - " + std::to_string(i));

                return false;
            }

            faceVertices.push_back(descr);
        }

        fd_t fd = halfedgeMesh.add_face(faceVertices);

        if (fd == hmesh_t::null_face()) {
            // Hint: this can happen when the mesh does not have a consistent
            // winding order i.e. some faces are CCW and others are CW
            context_uptr->log(MC_DEBUG_SOURCE_API, MC_DEBUG_TYPE_ERROR, 0, MC_DEBUG_SEVERITY_HIGH, "non-manifold edge on face " + std::to_string(i));

            return false;
        }

        faceSizeOffset += face_vertex_count;
    }
#endif
    TIMESTACK_POP();

    TIMESTACK_POP();

    return true;
}

#if 0
// this function converts a halfedge mesh representation (from the kernel
// backend) to an index array mesh (for the user).
void hmesh_to_array_mesh(
#if defined(MCUT_MULTI_THREADED)
    const std::unique_ptr<context_t>& context_uptr,
#endif
    array_mesh_t& indexArrayMesh,
    const output_mesh_info_t& halfedgeMeshInfo,
    const std::unordered_map<vd_t, vec3>& addedFpPartitioningVerticesOnCorrespondingInputSrcMesh,
    const std::unordered_map<fd_t, fd_t>& fpPartitionChildFaceToCorrespondingInputSrcMeshFace,
    const std::unordered_map<vd_t, vec3>& addedFpPartitioningVerticesOnCorrespondingInputCutMesh,
    const std::unordered_map<fd_t, fd_t>& fpPartitionChildFaceToCorrespondingInputCutMeshFace,
    const int client_sourcemesh_vertex_count,
    const int client_sourcemesh_face_count,
    const int internal_sourcemesh_vertex_count,
    const int internal_sourcemesh_face_count)
{
    SCOPED_TIMER(__FUNCTION__);

    //
    // vertices
    //
    TIMESTACK_PUSH("Add vertices");
    // create the vertices

    // number of vertices is the same irrespective of whether we are dealing with a
    // triangulated mesh instance or not. Thus, only one set of vertices is stored

    indexArrayMesh.numVertices = halfedgeMeshInfo.mesh.number_of_vertices();

    MCUT_ASSERT(indexArrayMesh.numVertices >= 3);

    indexArrayMesh.pVertices = std::unique_ptr<double[]>(new double[(std::size_t)indexArrayMesh.numVertices * 3u]);

    if (!halfedgeMeshInfo.data_maps.vertex_map.empty()) {
        indexArrayMesh.pVertexMapIndices = std::unique_ptr<uint32_t[]>(new uint32_t[indexArrayMesh.numVertices]);
    }

#if defined(MCUT_MULTI_THREADED)
    {
        typedef vertex_array_iterator_t InputStorageIteratorType;
        typedef int OutputStorageType;

        auto fn_copy_vertices = [&](InputStorageIteratorType block_start_, InputStorageIteratorType block_end_) -> OutputStorageType {
            for (InputStorageIteratorType viter = block_start_; viter != block_end_; ++viter) {
                const vec3& point = halfedgeMeshInfo.mesh.vertex(*viter);
                const uint32_t i = (uint32_t)std::distance(halfedgeMeshInfo.mesh.vertices_begin(), viter);

                indexArrayMesh.pVertices[((size_t)i * 3u) + 0u] = point.x();
                indexArrayMesh.pVertices[((size_t)i * 3u) + 1u] = point.y();
                indexArrayMesh.pVertices[((size_t)i * 3u) + 2u] = point.z();

                if (!halfedgeMeshInfo.data_maps.vertex_map.empty()) {
                    MCUT_ASSERT((size_t)*viter < halfedgeMeshInfo.data_maps.vertex_map.size() /*halfedgeMeshInfo.data_maps.vertex_map.count(*vIter) == 1*/);

                    uint32_t internalInputMeshVertexDescr = SAFE_ACCESS(halfedgeMeshInfo.data_maps.vertex_map, *viter);
                    uint32_t userInputMeshVertexDescr = UINT32_MAX;
                    bool internalInputMeshVertexDescrIsForIntersectionPoint = (internalInputMeshVertexDescr == UINT32_MAX);

                    if (!internalInputMeshVertexDescrIsForIntersectionPoint) { // user-mesh vertex or vertex that is added due to face-partitioning
                        bool vertexExistsDueToFacePartition = false;
                        const bool internalInputMeshVertexDescrIsForSrcMesh = ((int)internalInputMeshVertexDescr < internal_sourcemesh_vertex_count);

                        if (internalInputMeshVertexDescrIsForSrcMesh) {
                            std::unordered_map<vd_t, vec3>::const_iterator fiter = addedFpPartitioningVerticesOnCorrespondingInputSrcMesh.find(vd_t(internalInputMeshVertexDescr));
                            vertexExistsDueToFacePartition = (fiter != addedFpPartitioningVerticesOnCorrespondingInputSrcMesh.cend());
                        } else // internalInputMeshVertexDescrIsForCutMesh
                        {
                            std::unordered_map<vd_t, vec3>::const_iterator fiter = addedFpPartitioningVerticesOnCorrespondingInputCutMesh.find(vd_t(internalInputMeshVertexDescr));
                            vertexExistsDueToFacePartition = (fiter != addedFpPartitioningVerticesOnCorrespondingInputCutMesh.cend());
                        }

                        if (!vertexExistsDueToFacePartition) { // user-mesh vertex

                            MCUT_ASSERT(internal_sourcemesh_vertex_count > 0);

                            if (!internalInputMeshVertexDescrIsForSrcMesh) // is it a cut-mesh vertex discriptor ..?
                            {
                                const uint32_t internalInputMeshVertexDescrNoOffset = (internalInputMeshVertexDescr - internal_sourcemesh_vertex_count);
                                userInputMeshVertexDescr = (internalInputMeshVertexDescrNoOffset + client_sourcemesh_vertex_count); // ensure that we offset using number of [user-provided mesh] vertices
                            } else {
                                userInputMeshVertexDescr = internalInputMeshVertexDescr; // src-mesh vertices have no offset unlike cut-mesh vertices
                            }
                        }
                    }

                    indexArrayMesh.pVertexMapIndices[i] = userInputMeshVertexDescr;
                }
            }
            return 0;
        };

        std::vector<std::future<int>> futures;
        int _1;

        parallel_fork_and_join(
            context_uptr->scheduler,
            halfedgeMeshInfo.mesh.vertices_begin(),
            halfedgeMeshInfo.mesh.vertices_end(),
            (1 << 8),
            fn_copy_vertices,
            _1, // out
            futures);

        for (int i = 0; i < (int)futures.size(); ++i) {
            std::future<int>& f = futures[i];
            MCUT_ASSERT(f.valid());
            f.wait(); // simply wait for result to be done
        }
    }
#else // #if defined(MCUT_MULTI_THREADED)

    for (uint32_t i = 0; i < indexArrayMesh.numVertices; ++i) {

        // vertex_array_iterator_t vIter = halfedgeMeshInfo.mesh.vertices_begin();
        // std::advance(vIter, i);
        vd_t vdescr(i);
        const vec3& point = halfedgeMeshInfo.mesh.vertex(vdescr /**vIter*/);

        indexArrayMesh.pVertices[((size_t)i * 3u) + 0u] = point.x();
        indexArrayMesh.pVertices[((size_t)i * 3u) + 1u] = point.y();
        indexArrayMesh.pVertices[((size_t)i * 3u) + 2u] = point.z();

        // std::cout << indexArrayMesh.pVertices[(i * 3u) + 0u] << " " << indexArrayMesh.pVertices[(i * 3u) + 1u] << " " << indexArrayMesh.pVertices[(i * 3u) + 2u] << std::endl;

        // vmap[*vIter] = i;

        if (!halfedgeMeshInfo.data_maps.vertex_map.empty()) {
            MCUT_ASSERT((size_t)i < halfedgeMeshInfo.data_maps.vertex_map.size() /*halfedgeMeshInfo.data_maps.vertex_map.count(*vIter) == 1*/);

            // Here we use whatever value was assigned to the current vertex by the kernel.
            // Vertices that are polygon intersection points have a value of uint_max i.e. null_vertex().
            uint32_t internalInputMeshVertexDescr = SAFE_ACCESS(halfedgeMeshInfo.data_maps.vertex_map, vdescr /**vIter*/);
            // We use the same default value as that used by the kernel for intersection
            // points (intersection points at mapped to uint_max i.e. null_vertex())
            uint32_t userInputMeshVertexDescr = UINT32_MAX;
            // This is true only for polygon intersection points computed by the kernel
            bool internalInputMeshVertexDescrIsForIntersectionPoint = (internalInputMeshVertexDescr == UINT32_MAX);

            if (!internalInputMeshVertexDescrIsForIntersectionPoint) { // user-mesh vertex or vertex that is added due to face-partitioning
                // NOTE: The kernel will assign/map a 'proper' index value to vertices that exist due to face partitioning.
                // 'proper' here means that the kernel treats these vertices as 'original vertices' from a user-provided input
                // mesh. In reality, we added such vertices in order to partition a face. i.e. the kernel is not aware
                // that a given input mesh it is working with is modified.
                // So, here we have to fix that mapping information to correctly state that "any vertex added due to face
                // partitioning was not in the user provided input mesh" and should therefore be treated/labelled as an intersection
                // point i.e. it should map to UINT32_MAX because it does not map to any vertex in the user provided input mesh.
                bool vertexExistsDueToFacePartition = false;
                const bool internalInputMeshVertexDescrIsForSrcMesh = ((int)internalInputMeshVertexDescr < internal_sourcemesh_vertex_count);

                if (internalInputMeshVertexDescrIsForSrcMesh) {
                    std::unordered_map<vd_t, vec3>::const_iterator fiter = addedFpPartitioningVerticesOnCorrespondingInputSrcMesh.find(vd_t(internalInputMeshVertexDescr));
                    vertexExistsDueToFacePartition = (fiter != addedFpPartitioningVerticesOnCorrespondingInputSrcMesh.cend());
                } else // internalInputMeshVertexDescrIsForCutMesh
                {
                    std::unordered_map<vd_t, vec3>::const_iterator fiter = addedFpPartitioningVerticesOnCorrespondingInputCutMesh.find(vd_t(internalInputMeshVertexDescr));
                    vertexExistsDueToFacePartition = (fiter != addedFpPartitioningVerticesOnCorrespondingInputCutMesh.cend());
                }

                if (!vertexExistsDueToFacePartition) { // user-mesh vertex

                    MCUT_ASSERT(internal_sourcemesh_vertex_count > 0);

                    if (!internalInputMeshVertexDescrIsForSrcMesh) // is it a cut-mesh vertex discriptor ..?
                    {

                        // vertices added due to face-partitioning will have an unoffsetted index/descr that is >= client_sourcemesh_vertex_count
                        const uint32_t internalInputMeshVertexDescrNoOffset = (internalInputMeshVertexDescr - internal_sourcemesh_vertex_count);

                        // if (internalInputMeshVertexDescrNoOffset < userCutMeshVertexCount) {
                        // const int offset_descrepancy = (internal_sourcemesh_vertex_count - client_sourcemesh_vertex_count);
                        userInputMeshVertexDescr = (internalInputMeshVertexDescrNoOffset + client_sourcemesh_vertex_count); // ensure that we offset using number of [user-provided mesh] vertices
                        //}
                    } else {
                        // if (internalInputMeshVertexDescr < client_sourcemesh_vertex_count) {
                        // const int offset_descrepancy = (internal_sourcemesh_vertex_count - client_sourcemesh_vertex_count);
                        userInputMeshVertexDescr = internalInputMeshVertexDescr; // src-mesh vertices have no offset unlike cut-mesh vertices
                        //}
                    }
                }
            }

            indexArrayMesh.pVertexMapIndices[i] = userInputMeshVertexDescr;
        }
    }
#endif
    // MCUT_ASSERT(!vmap.empty());

    TIMESTACK_POP();

    // create array of seam vertices

    TIMESTACK_PUSH("Create seam vertices");
    uint32_t numSeamVertexIndices = (uint32_t)halfedgeMeshInfo.seam_vertices.size();
    indexArrayMesh.numSeamVertexIndices = numSeamVertexIndices;
    if (indexArrayMesh.numSeamVertexIndices > 0u) {
        indexArrayMesh.pSeamVertexIndices = std::unique_ptr<uint32_t[]>(new uint32_t[numSeamVertexIndices]);
        for (uint32_t i = 0; i < numSeamVertexIndices; ++i) {
            indexArrayMesh.pSeamVertexIndices[i] = halfedgeMeshInfo.seam_vertices[i];
        }
    }
    TIMESTACK_POP();

    //
    // TODO: add functionality to add seam edges
    //

    //
    // faces
    //

    TIMESTACK_PUSH("Create faces");

    indexArrayMesh.numFaces = halfedgeMeshInfo.mesh.number_of_faces();

    MCUT_ASSERT(indexArrayMesh.numFaces > 0);

    indexArrayMesh.pFaceSizes = std::unique_ptr<uint32_t[]>(new uint32_t[indexArrayMesh.numFaces]);

    if (!halfedgeMeshInfo.data_maps.face_map.empty()) {
        indexArrayMesh.pFaceMapIndices = std::unique_ptr<uint32_t[]>(new uint32_t[indexArrayMesh.numFaces]);
    }

    indexArrayMesh.pFaceAdjFacesSizes = std::unique_ptr<uint32_t[]>(new uint32_t[indexArrayMesh.numFaces]);

    //
    // Here, we collect size information about faces
    //
    std::vector<std::vector<fd_t>> gatheredFacesAdjFaces(indexArrayMesh.numFaces);
    std::vector<std::vector<vd_t>> gatheredFaces(indexArrayMesh.numFaces);

#if defined(MCUT_MULTI_THREADED)
    {
        typedef face_array_iterator_t InputStorageIteratorType;
        typedef int OutputStorageType;

        auto fn_copy_face_info0 = [&](InputStorageIteratorType block_start_, InputStorageIteratorType block_end_) -> OutputStorageType {
            for (InputStorageIteratorType i = block_start_; i != block_end_; ++i) {
                const uint32_t faceID = (uint32_t)std::distance(halfedgeMeshInfo.mesh.faces_begin(), i);

                {
                    std::vector<vd_t> vertices_around_face = halfedgeMeshInfo.mesh.get_vertices_around_face(*i);
                    indexArrayMesh.pFaceSizes[faceID] = (uint32_t)vertices_around_face.size();
                    gatheredFaces[faceID] = std::move(vertices_around_face);
                }

                {
                    std::vector<fd_t> adjFaces = halfedgeMeshInfo.mesh.get_faces_around_face(*i);
                    indexArrayMesh.pFaceAdjFacesSizes[faceID] = (uint32_t)adjFaces.size();
                    gatheredFacesAdjFaces[*i] = std::move(adjFaces);
                }

                if (!halfedgeMeshInfo.data_maps.face_map.empty()) {
                    MCUT_ASSERT((size_t)*i < halfedgeMeshInfo.data_maps.face_map.size() /*halfedgeMeshInfo.data_maps.face_map.count(*i) == 1*/);

                    uint32_t internal_inputmesh_face_idx = (uint32_t)SAFE_ACCESS(halfedgeMeshInfo.data_maps.face_map, *i);
                    uint32_t client_inputmesh_face_idx = INT32_MAX;
                    const bool internal_inputmesh_face_idx_is_for_src_mesh = ((int)internal_inputmesh_face_idx < internal_sourcemesh_face_count);

                    if (internal_inputmesh_face_idx_is_for_src_mesh) {
                        std::unordered_map<fd_t, fd_t>::const_iterator fiter = fpPartitionChildFaceToCorrespondingInputSrcMeshFace.find(fd_t(internal_inputmesh_face_idx));
                        if (fiter != fpPartitionChildFaceToCorrespondingInputSrcMeshFace.cend()) {
                            client_inputmesh_face_idx = fiter->second;
                        } else {
                            client_inputmesh_face_idx = internal_inputmesh_face_idx;
                        }
                        MCUT_ASSERT((int)client_inputmesh_face_idx < (int)client_sourcemesh_face_count);
                    } else // internalInputMeshVertexDescrIsForCutMesh
                    {
                        std::unordered_map<fd_t, fd_t>::const_iterator fiter = fpPartitionChildFaceToCorrespondingInputCutMeshFace.find(fd_t(internal_inputmesh_face_idx));
                        if (fiter != fpPartitionChildFaceToCorrespondingInputCutMeshFace.cend()) {
                            uint32_t index_without_offset = (fiter->second - internal_sourcemesh_face_count);
                            client_inputmesh_face_idx = index_without_offset + client_sourcemesh_face_count;
                        } else {
                            uint32_t index_without_offset = (internal_inputmesh_face_idx - internal_sourcemesh_face_count);
                            client_inputmesh_face_idx = index_without_offset + client_sourcemesh_face_count;
                        }
                    }

                    MCUT_ASSERT(client_inputmesh_face_idx != INT32_MAX);

                    indexArrayMesh.pFaceMapIndices[(uint32_t)(*i)] = client_inputmesh_face_idx;
                } // if (!halfedgeMeshInfo.data_maps.face_map.empty()) {
            }
            return 0;
        };
        std::vector<std::future<int>> futures;
        int _1;

        auto vvv = halfedgeMeshInfo.mesh.vertices_begin();
        ;
        std::advance(vvv, 1);
        auto hhh = halfedgeMeshInfo.mesh.halfedges_begin();
        ;
        hhh += 1;
        // std::advance(hhh, 1);
        face_array_iterator_t fff = halfedgeMeshInfo.mesh.faces_begin();
        fff += 1;
        // std::advance(fff, (std::size_t)1);

        parallel_fork_and_join(
            context_uptr->scheduler,
            halfedgeMeshInfo.mesh.faces_begin(),
            halfedgeMeshInfo.mesh.faces_end(),
            (1 << 7),
            fn_copy_face_info0,
            _1, // out
            futures);

        for (int i = 0; i < (int)futures.size(); ++i) {
            std::future<int>& f = futures[i];
            MCUT_ASSERT(f.valid());
            f.wait(); // simply wait for result to be done
        }
    }
#else // #if defined(MCUT_MULTI_THREADED)

    int faceID = 0; // std::distance(halfedgeMeshInfo.mesh.faces_begin(), i);
    for (face_array_iterator_t i = halfedgeMeshInfo.mesh.faces_begin(); i != halfedgeMeshInfo.mesh.faces_end(); ++i) {
        // const int faceID = std::distance(halfedgeMeshInfo.mesh.faces_begin(), i);

        {
            std::vector<vd_t> vertices_around_face = halfedgeMeshInfo.mesh.get_vertices_around_face(*i);
            indexArrayMesh.pFaceSizes[faceID] = (uint32_t)vertices_around_face.size();
            gatheredFaces[faceID] = std::move(vertices_around_face);
        }

        {
            std::vector<fd_t> adjFaces = halfedgeMeshInfo.mesh.get_faces_around_face(*i);
            indexArrayMesh.pFaceAdjFacesSizes[faceID] = (uint32_t)adjFaces.size();
            gatheredFacesAdjFaces[*i] = std::move(adjFaces);
        }

        if (!halfedgeMeshInfo.data_maps.face_map.empty()) {
            MCUT_ASSERT((size_t)*i < halfedgeMeshInfo.data_maps.face_map.size() /*halfedgeMeshInfo.data_maps.face_map.count(*i) == 1*/);

            uint32_t internal_inputmesh_face_idx = (uint32_t)SAFE_ACCESS(halfedgeMeshInfo.data_maps.face_map, *i);
            uint32_t client_inputmesh_face_idx = INT32_MAX;
            const bool internal_inputmesh_face_idx_is_for_src_mesh = ((int)internal_inputmesh_face_idx < internal_sourcemesh_face_count);

            if (internal_inputmesh_face_idx_is_for_src_mesh) {
                std::unordered_map<fd_t, fd_t>::const_iterator fiter = fpPartitionChildFaceToCorrespondingInputSrcMeshFace.find(fd_t(internal_inputmesh_face_idx));
                if (fiter != fpPartitionChildFaceToCorrespondingInputSrcMeshFace.cend()) {
                    client_inputmesh_face_idx = fiter->second;
                } else {
                    client_inputmesh_face_idx = internal_inputmesh_face_idx;
                }
                MCUT_ASSERT((int)client_inputmesh_face_idx < (int)client_sourcemesh_face_count);
            } else // internalInputMeshVertexDescrIsForCutMesh
            {
                std::unordered_map<fd_t, fd_t>::const_iterator fiter = fpPartitionChildFaceToCorrespondingInputCutMeshFace.find(fd_t(internal_inputmesh_face_idx));
                if (fiter != fpPartitionChildFaceToCorrespondingInputCutMeshFace.cend()) {
                    uint32_t index_without_offset = (fiter->second - internal_sourcemesh_face_count);
                    client_inputmesh_face_idx = index_without_offset + client_sourcemesh_face_count;
                } else {
                    uint32_t index_without_offset = (internal_inputmesh_face_idx - internal_sourcemesh_face_count);
                    client_inputmesh_face_idx = index_without_offset + client_sourcemesh_face_count;
                }
            }

            MCUT_ASSERT(client_inputmesh_face_idx != INT32_MAX);

            indexArrayMesh.pFaceMapIndices[(uint32_t)(*i)] = client_inputmesh_face_idx;
        } // if (!halfedgeMeshInfo.data_maps.face_map.empty()) {

        faceID++;
    }
#endif //#if defined(MCUT_MULTI_THREADED)
    MCUT_ASSERT(gatheredFacesAdjFaces.size() == indexArrayMesh.numFaces); // sanity check

    //
    // Here, we store information about faces (vertex indices, adjacent faces etc.)
    //

    std::vector<uint32_t> adjFaceArrayPartialSums(indexArrayMesh.numFaces, 0);
    std::partial_sum( //
        indexArrayMesh.pFaceAdjFacesSizes.get(), //
        indexArrayMesh.pFaceAdjFacesSizes.get() + indexArrayMesh.numFaces, //
        adjFaceArrayPartialSums.data());

    indexArrayMesh.numFaceAdjFaceIndices = adjFaceArrayPartialSums.back();
    indexArrayMesh.pFaceAdjFaces = std::unique_ptr<uint32_t[]>(new uint32_t[indexArrayMesh.numFaceAdjFaceIndices]);

    std::vector<uint32_t> faceIndicesArrayPartialSums(indexArrayMesh.numFaces, 0);
    std::partial_sum( //
        indexArrayMesh.pFaceSizes.get(), //
        indexArrayMesh.pFaceSizes.get() + indexArrayMesh.numFaces, //
        faceIndicesArrayPartialSums.data());

    indexArrayMesh.numFaceIndices = faceIndicesArrayPartialSums.back();
    indexArrayMesh.pFaceIndices = std::unique_ptr<uint32_t[]>(new uint32_t[indexArrayMesh.numFaceIndices]);

#if defined(MCUT_MULTI_THREADED)
    {
        typedef face_array_iterator_t InputStorageIteratorType;
        typedef int OutputStorageType;

        auto fn_copy_face_info1 = [&](InputStorageIteratorType block_start_, InputStorageIteratorType block_end_) -> OutputStorageType {
            for (InputStorageIteratorType i = block_start_; i != block_end_; ++i) {
                const uint32_t faceID = (uint32_t)std::distance(halfedgeMeshInfo.mesh.faces_begin(), i);
                { // store face-vertex indices
                    const std::vector<vd_t>& faceVertices = gatheredFaces[faceID];
                    const uint32_t faceSize = (uint32_t)faceVertices.size();
                    const int faceVertexIndexOffset = faceIndicesArrayPartialSums[faceID] - faceSize;

                    for (uint32_t j = 0; j < faceSize; ++j) {
                        const vd_t vd = faceVertices[j];
                        indexArrayMesh.pFaceIndices[(size_t)faceVertexIndexOffset + j] = (uint32_t)vd; // vmap[vd];
                    }
                }

                { // store adjacent-face indices
                    const std::vector<fd_t>& faceAdjFaces = gatheredFacesAdjFaces[faceID];
                    const uint32_t adjFacesSize = (uint32_t)faceAdjFaces.size();
                    const int faceAdjFaceIndexOffset = adjFaceArrayPartialSums[faceID] - adjFacesSize;

                    for (uint32_t j = 0; j < adjFacesSize; ++j) {
                        const fd_t adjFace = faceAdjFaces[j];
                        indexArrayMesh.pFaceAdjFaces[(size_t)faceAdjFaceIndexOffset + j] = (uint32_t)adjFace;
                    }
                }
            }
            return 0;
        };

        std::vector<std::future<int>> futures;
        int _1;

        parallel_fork_and_join(
            context_uptr->scheduler,
            halfedgeMeshInfo.mesh.faces_begin(),
            halfedgeMeshInfo.mesh.faces_end(),
            (1 << 8),
            fn_copy_face_info1,
            _1, // out
            futures);

        for (int i = 0; i < (int)futures.size(); ++i) {
            std::future<int>& f = futures[i];
            MCUT_ASSERT(f.valid());
            f.wait(); // simply wait for result to be done
        }
    }
#else // #if defined(MCUT_MULTI_THREADED)
    faceID = 0; // std::distance(halfedgeMeshInfo.mesh.faces_begin(), i);
    // for each face
    for (face_array_iterator_t i = halfedgeMeshInfo.mesh.faces_begin(); i != halfedgeMeshInfo.mesh.faces_end(); ++i) {

        { // store face-vertex indices
            const std::vector<vd_t>& faceVertices = gatheredFaces[faceID];
            const uint32_t faceSize = (uint32_t)faceVertices.size();
            const int faceVertexIndexOffset = faceIndicesArrayPartialSums[faceID] - faceSize;

            for (uint32_t j = 0; j < faceSize; ++j) {
                const vd_t vd = faceVertices[j];
                indexArrayMesh.pFaceIndices[(size_t)faceVertexIndexOffset + j] = (uint32_t)vd; // vmap[vd];
            }
        }

        { // store adjacent-face indices
            const std::vector<fd_t>& faceAdjFaces = gatheredFacesAdjFaces[faceID];
            const uint32_t adjFacesSize = (uint32_t)faceAdjFaces.size();
            const int faceAdjFaceIndexOffset = adjFaceArrayPartialSums[faceID] - adjFacesSize;

            for (uint32_t j = 0; j < adjFacesSize; ++j) {
                const fd_t adjFace = faceAdjFaces[j];
                indexArrayMesh.pFaceAdjFaces[(size_t)faceAdjFaceIndexOffset + j] = (uint32_t)adjFace;
            }
        }

        faceID++;
    }
#endif
    TIMESTACK_POP();

    //
    // edges
    //

    TIMESTACK_PUSH("Create edges");
    indexArrayMesh.numEdgeIndices = halfedgeMeshInfo.mesh.number_of_edges() * 2;

    MCUT_ASSERT(indexArrayMesh.numEdgeIndices > 0);
    indexArrayMesh.pEdges = std::unique_ptr<uint32_t[]>(new uint32_t[indexArrayMesh.numEdgeIndices]);

    // std::vector<std::pair<vd_t, vd_t>> gatheredEdges;
#if defined(MCUT_MULTI_THREADED)
    {
        typedef edge_array_iterator_t InputStorageIteratorType;
        typedef int OutputStorageType;

        auto fn_copy_edges = [&](InputStorageIteratorType block_start_, InputStorageIteratorType block_end_) -> OutputStorageType {
            // uint32_t bs =*block_start_;
            // uint32_t be =*block_end_;

            for (InputStorageIteratorType eiter = block_start_; eiter != block_end_; ++eiter) {
                // printf("block_start_=%u; block_end_=%u eiter=%u\n", (uint32_t)*block_start_,  (uint32_t)*block_end_, (uint32_t)*eiter);
                // bool is_end = eiter == block_end_;
                // uint32_t edge_id = std::distance(halfedgeMeshInfo.mesh.edges_begin(), eiter);
                vd_t v0 = halfedgeMeshInfo.mesh.vertex(*eiter, 0);
                vd_t v1 = halfedgeMeshInfo.mesh.vertex(*eiter, 1);

                // uint32_t r = halfedgeMeshInfo.mesh.count_removed_elements_in_range(halfedgeMeshInfo.mesh.edges_begin(), eiter);
                //  NOTE: our override of std::distance accounts for removed elements
                uint32_t edge_idx = (uint32_t)std::distance(halfedgeMeshInfo.mesh.edges_begin(), eiter); // - r;
                // printf("edge_idx = %d (%u)\n",edge_idx, (uint32_t)*eiter );
                // MCUT_ASSERT((size_t)v0 < vmap.size());
                MCUT_ASSERT(((size_t)edge_idx * 2u) + 0u < indexArrayMesh.numEdgeIndices);
                indexArrayMesh.pEdges[((size_t)edge_idx * 2u) + 0u] = (uint32_t)v0; // vmap[v0];
                // MCUT_ASSERT((size_t)v1 < vmap.size());
                MCUT_ASSERT(((size_t)edge_idx * 2u) + 1u < indexArrayMesh.numEdgeIndices);
                indexArrayMesh.pEdges[((size_t)edge_idx * 2u) + 1u] = (uint32_t)v1; //  vmap[v1];
            }

            return 0;
        };

        std::vector<std::future<int>> futures;
        int _1;

        parallel_fork_and_join(
            context_uptr->scheduler,
            halfedgeMeshInfo.mesh.edges_begin(),
            halfedgeMeshInfo.mesh.edges_end(),
            (1 << 8),
            fn_copy_edges,
            _1, // out
            futures);

        for (int i = 0; i < (int)futures.size(); ++i) {
            std::future<int>& f = futures[i];
            MCUT_ASSERT(f.valid());
            f.wait(); // simply wait for result to be done
        }
    }
#else // #if defined(MCUT_MULTI_THREADED)
    // note: cannot use std::distance with halfedge mesh iterators
    // not implemented because it'd be too slow
    uint32_t edge_idx = 0; // std::distance(halfedgeMeshInfo.mesh.edges_begin(), i);

    for (edge_array_iterator_t i = halfedgeMeshInfo.mesh.edges_begin(); i != halfedgeMeshInfo.mesh.edges_end(); ++i) {

        vd_t v0 = halfedgeMeshInfo.mesh.vertex(*i, 0);
        vd_t v1 = halfedgeMeshInfo.mesh.vertex(*i, 1);

        // gatheredEdges.emplace_back(v0, v1);
        MCUT_ASSERT(((size_t)edge_idx * 2u) + 0u < indexArrayMesh.numEdgeIndices);
        // MCUT_ASSERT((size_t)v0 < vmap.size());
        indexArrayMesh.pEdges[((size_t)edge_idx * 2u) + 0u] = (uint32_t)v0; // vmap[v0];
        MCUT_ASSERT(((size_t)edge_idx * 2u) + 1u < indexArrayMesh.numEdgeIndices);
        // MCUT_ASSERT((size_t)v1 < vmap.size());
        indexArrayMesh.pEdges[((size_t)edge_idx * 2u) + 1u] = (uint32_t)v1; // vmap[v1];

        edge_idx++;
    }
#endif
#if 0
    // sanity check

    MCUT_ASSERT(gatheredEdges.size() == indexArrayMesh.numEdgeIndices / 2);

    indexArrayMesh.pEdges = std::unique_ptr<uint32_t[]>(new uint32_t[indexArrayMesh.numEdgeIndices]);

    for (uint32_t i = 0; i < (uint32_t)gatheredEdges.size(); ++i)
    {
        const std::pair<vd_t, vd_t> &edge = gatheredEdges[i];
        vd_t v0 = edge.first;
        vd_t v1 = edge.second;

        MCUT_ASSERT((size_t)v0 < vmap.size());
        indexArrayMesh.pEdges[((size_t)i * 2u) + 0u] = vmap[v0];
        MCUT_ASSERT((size_t)v1 < vmap.size());
        indexArrayMesh.pEdges[((size_t)i * 2u) + 1u] = vmap[v1];
    }
#endif
    TIMESTACK_POP();
}
#endif

bool is_coplanar(const hmesh_t& m, const fd_t& f, int& fv_count)
{
    const std::vector<vd_t> vertices = m.get_vertices_around_face(f);
    fv_count = (int)vertices.size();
    if (fv_count > 3) // non-triangle
    {
        for (int i = 0; i < (fv_count - 3); ++i) {
            const int j = (i + 1) % fv_count;
            const int k = (i + 2) % fv_count;
            const int l = (i + 3) % fv_count;

            const vd_t& vi = vertices[i];
            const vd_t& vj = vertices[j];
            const vd_t& vk = vertices[k];
            const vd_t& vl = vertices[l];

            const vec3& vi_coords = m.vertex(vi);
            const vec3& vj_coords = m.vertex(vj);
            const vec3& vk_coords = m.vertex(vk);
            const vec3& vl_coords = m.vertex(vl);

            const bool are_coplaner = coplaner(vi_coords, vj_coords, vk_coords, vl_coords);

            if (!are_coplaner) {
                return false;
            }
        }
    }
    return true;
}

// check that the halfedge-mesh version of a user-provided mesh is valid (i.e.
// it is a non-manifold mesh containing a single connected component etc.)
bool check_input_mesh(std::unique_ptr<context_t>& context_uptr, const hmesh_t& m)
{
    if (m.number_of_vertices() < 3) {
        context_uptr->log(
            MC_DEBUG_SOURCE_API,
            MC_DEBUG_TYPE_ERROR,
            0,
            MC_DEBUG_SEVERITY_HIGH,
            "Invalid vertex count (V=" + std::to_string(m.number_of_vertices()) + ")");
        return false;
    }

    if (m.number_of_faces() < 1) {
        context_uptr->log(
            MC_DEBUG_SOURCE_API,
            MC_DEBUG_TYPE_ERROR,
            0,
            MC_DEBUG_SEVERITY_HIGH,
            "Invalid face count (F=" + std::to_string(m.number_of_faces()) + ")");
        return false;
    }

    std::vector<int> fccmap;
    std::vector<int> cc_to_vertex_count;
    std::vector<int> cc_to_face_count;
    int n = find_connected_components(fccmap, m, cc_to_vertex_count, cc_to_face_count);

    if (n != 1) {
        context_uptr->log(
            MC_DEBUG_SOURCE_API,
            MC_DEBUG_TYPE_ERROR,
            0,
            MC_DEBUG_SEVERITY_HIGH,
            "Detected multiple connected components in mesh (N=" + std::to_string(n) + ")");
        return false;
    }

    // check that the vertices of each face are co-planar
    for (face_array_iterator_t f = m.faces_begin(); f != m.faces_end(); ++f) {
        int fv_count = 0;
        const bool face_is_coplanar = is_coplanar(m, *f, fv_count);
        if (!face_is_coplanar) {
            context_uptr->log(
                MC_DEBUG_SOURCE_API,
                MC_DEBUG_TYPE_OTHER,
                0,
                MC_DEBUG_SEVERITY_NOTIFICATION,
                "Vertices (" + std::to_string(fv_count) + ") on face " + std::to_string(*f) + " not coplanar");
            // No need to return false, simply warn. It is difficult to
            // know whether the non-coplanarity is severe enough to cause
            // confusion when computing intersection points between two
            // polygons (min=2 but sometimes can get 1 due to non-coplanarity
            // of face vertices).
            // In general, the more vertices on a face, the less likely
            // they are to be co-planar. Faces with a low number of polygons
            // are ideal (3 vertices being the best)
            // result = false;
            // break;
        }
    }

    return true;
}

McResult convert(const status_t& v)
{
    McResult result = McResult::MC_RESULT_MAX_ENUM;
    switch (v) {
    case status_t::SUCCESS:
        result = McResult::MC_NO_ERROR;
        break;
    case status_t::GENERAL_POSITION_VIOLATION:
    case status_t::INVALID_MESH_INTERSECTION:
        result = McResult::MC_INVALID_OPERATION;
        break;
    default:
        std::fprintf(stderr, "[MCUT]: warning - conversion error (McResult=%d)\n", (int)v);
    }
    return result;
}

McPatchLocation convert(const cm_patch_location_t& v)
{
    McPatchLocation result = McPatchLocation::MC_PATCH_LOCATION_ALL;
    switch (v) {
    case cm_patch_location_t::INSIDE:
        result = McPatchLocation::MC_PATCH_LOCATION_INSIDE;
        break;
    case cm_patch_location_t::OUTSIDE:
        result = McPatchLocation::MC_PATCH_LOCATION_OUTSIDE;
        break;
    case cm_patch_location_t::UNDEFINED:
        result = McPatchLocation::MC_PATCH_LOCATION_UNDEFINED;
        break;
    default:
        std::fprintf(stderr, "[MCUT]: warning - conversion error (McPatchLocation)\n");
    }
    return result;
}

McFragmentLocation convert(const sm_frag_location_t& v)
{
    McFragmentLocation result = McFragmentLocation::MC_FRAGMENT_LOCATION_ALL;
    switch (v) {
    case sm_frag_location_t::ABOVE:
        result = McFragmentLocation::MC_FRAGMENT_LOCATION_ABOVE;
        break;
    case sm_frag_location_t::BELOW:
        result = McFragmentLocation::MC_FRAGMENT_LOCATION_BELOW;
        break;
    case sm_frag_location_t::UNDEFINED:
        result = McFragmentLocation::MC_FRAGMENT_LOCATION_UNDEFINED;
        break;
    default:
        std::fprintf(stderr, "[MCUT]: warning - conversion error (McFragmentLocation)\n");
    }
    return result;
}

void resolve_floating_polygons(
    bool& source_hmesh_modified,
    bool& cut_hmesh_modified,
    const std::map<fd_t /*input mesh face with fp*/, std::vector<floating_polygon_info_t> /*list of floating polys*/>& detected_floating_polygons,
    const int source_hmesh_face_count_prev,
    hmesh_t& source_hmesh,
    hmesh_t& cut_hmesh,
    std::unordered_map<fd_t /*child face*/, fd_t /*parent face in the [user-provided] source mesh*/>& source_hmesh_child_to_usermesh_birth_face,
    std::unordered_map<fd_t /*child face*/, fd_t /*parent face in the [user-provided] cut mesh*/>& cut_hmesh_child_to_usermesh_birth_face,
    std::unordered_map<vd_t, vec3>& source_hmesh_new_poly_partition_vertices,
    std::unordered_map<vd_t, vec3>& cut_hmesh_new_poly_partition_vertices)
{
    for (std::map<fd_t, std::vector<floating_polygon_info_t>>::const_iterator detected_floating_polygons_iter = detected_floating_polygons.cbegin();
         detected_floating_polygons_iter != detected_floating_polygons.cend();
         ++detected_floating_polygons_iter) {

        // get the [origin] input-mesh face index (Note: this index may be offsetted
        // to distinguish between source-mesh and cut-mesh faces).
        const fd_t parent_face_raw = detected_floating_polygons_iter->first;

        // NOTE: this boolean needs to be evaluated with "source_hmesh_face_count_prev" since the number of
        // src-mesh faces might change as we add more polygons due to partitioning.
        bool parent_face_from_source_hmesh = ((uint32_t)parent_face_raw < (uint32_t)source_hmesh_face_count_prev);

        // pointer to input mesh with face containing floating polygon
        // Note: this mesh will be modified as we add new faces.
        hmesh_t* parent_face_hmesh_ptr = (parent_face_from_source_hmesh ? &source_hmesh : &cut_hmesh);

        source_hmesh_modified = source_hmesh_modified || parent_face_from_source_hmesh;
        cut_hmesh_modified = cut_hmesh_modified || !parent_face_from_source_hmesh;

        // This data structure maps the new faces in the modified input mesh, to the original partitioned/parent face in the [user-provided] input mesh.
        std::unordered_map<fd_t, fd_t>& child_to_client_birth_face = (parent_face_from_source_hmesh ? source_hmesh_child_to_usermesh_birth_face : cut_hmesh_child_to_usermesh_birth_face);
        // This data structure stores the vertices added into the input mesh partition one or more face .
        // We store the coordinates here too because they are sometimes needed to performed perturbation.
        // This perturbation can happen when an input mesh face is partitioned with e.g. edge where that
        // is sufficient to resolve all floating polygons detected on that input mesh face.
        std::unordered_map<vd_t, vec3>& new_poly_partition_vertices = (parent_face_from_source_hmesh ? source_hmesh_new_poly_partition_vertices : cut_hmesh_new_poly_partition_vertices);

        // Now compute the actual input mesh face index (accounting for offset)
        // i.e. index/descriptor into the mesh referenced by "parent_face_hmesh_ptr"
        const fd_t parent_face = parent_face_from_source_hmesh ? parent_face_raw : fd_t((uint32_t)parent_face_raw - (uint32_t)source_hmesh_face_count_prev); // accounting for offset (NOTE: must updated "source_hmesh" state)

        MCUT_ASSERT(static_cast<uint32_t>(parent_face) < (uint32_t)parent_face_hmesh_ptr->number_of_faces());

        // for each floating polygon detected on current ps-face
        for (std::vector<floating_polygon_info_t>::const_iterator floating_poly_info_iter = detected_floating_polygons_iter->second.cbegin();
             floating_poly_info_iter != detected_floating_polygons_iter->second.cend();
             ++floating_poly_info_iter) {

            const floating_polygon_info_t& fpi = *floating_poly_info_iter;

            // ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
            // Here we now need to partition "origin_face" in "parent_face_hmesh_ptr"
            // by adding a new edge which is guarranteed to pass through the area
            // spanned by the floating polygon.

            // gather vertices of floating polygon (just a list of 3d coords provided by the kernel)

            const size_t floating_poly_vertex_count = fpi.polygon_vertices.size();
            MCUT_ASSERT(floating_poly_vertex_count >= 3);
            const size_t floating_poly_edge_count = floating_poly_vertex_count; // num edges is same as num verts

            // project the floating polygon to 2D

            std::vector<vec2> floating_poly_vertices_2d;

            project_to_2d(floating_poly_vertices_2d, fpi.polygon_vertices, fpi.polygon_normal, fpi.polygon_normal_largest_component);

            // face to be (potentially) partitioned
            // NOTE: This "origin_face" variable refer's to face that [may] actually be a child face that was created (in a previous iteration)
            // as a result of another partitioning.
            fd_t origin_face = parent_face;

            // we use this map to check is "origin_face" was actually created by polygon partitioning i.e. it did not exist in client/user-provided input mesh
            std::unordered_map<fd_t /*child*/, fd_t /*parent face in user/client provided input mesh*/>::const_iterator origin_to_birth_face_iter = child_to_client_birth_face.find(origin_face);

            // This boolean var will be true if "parent_face_raw" has more than one floating polygon associated with it, in which case
            // "parent_face_raw" may be unnecessarilly be partitioned once. In such a case we want to minimise the number of edges
            // that are used to partition "parent_face_raw" to as low as possible (minimum is one edge).
            // Thus, we need this boolean var to handle the cases where another partition of "parent_face_raw" (due to one of its other
            // floating polys) leads to the addition of another new edge (i.e. the one partitioning "parent_face_raw") that passes
            // through the current floating poly.
            // When this boolean variable is true, we will need to:
            // 1)   find all faces in "child_to_client_birth_face" that are mapped to same birth-face (in user provided mesh) as that
            //      of "parent_face_raw" (i.e. search by value)
            // 2)   for each such face check to see if any one of its edges intersect the current floating polygon
            // This is necessary to ensure a minimal set of partitions. See below for details.
            bool client_hmesh_birth_face_is_partitioned_atleast_once = (origin_to_birth_face_iter != child_to_client_birth_face.cend());
            fd_t client_hmesh_birth_face = hmesh_t::null_face();

            bool do_partition_current_face = true;

            // check if we still need to partition origin_face.
            // If a partition has already been made that added an edge into the mesh (i.e. the one referenced by "parent_face_hmesh_ptr")
            // which passes through the current floating poly, then we will not need to partition "parent_face_raw".
            // NOTE TO SELF: there is no guarrantee that the previously added edge that partitions "parent_face_raw" will not violate
            // general-position w.r.t the current floating poly.
            // Thus, general position might potentially be violated such that we would have to resort to numerical perturbation in the next
            // dispatch(...) call.
            if (client_hmesh_birth_face_is_partitioned_atleast_once) {

                client_hmesh_birth_face = origin_to_birth_face_iter->second;

                MCUT_ASSERT(origin_face == origin_to_birth_face_iter->first);

                // the child faces that we create by partitioning "client_hmesh_birth_face" (possibly over multiple dispatch calls
                // in the case that GP is violated by an added edge)
                std::vector<fd_t> faces_from_partitioned_birth_face;

                // for all other faces that share "client_hmesh_birth_face"
                for (std::unordered_map<fd_t, fd_t>::const_iterator it = child_to_client_birth_face.cbegin();
                     it != child_to_client_birth_face.cend();
                     ++it) {
                    if (it->second == client_hmesh_birth_face) { // matching client birth face ?
                        faces_from_partitioned_birth_face.push_back(it->first);
                    }
                }

                bool have_face_intersecting_fp = false;

                // Should it be the case that we must proceed to make [another] partition of the
                // birth-face, then "face_containing_floating_poly" represents the existing face (a child of the birth face)
                // in which the current floating polygon lies.
                fd_t face_containing_floating_poly = hmesh_t::null_face();

                // for each face sharing a client birth face with origin_face
                for (std::vector<fd_t>::const_iterator it = faces_from_partitioned_birth_face.cbegin();
                     it != faces_from_partitioned_birth_face.cend();
                     ++it) {

                    fd_t face = *it;

                    // ::::::::::::::::::::::
                    // get face vertex coords
                    const std::vector<vd_t> face_vertex_descriptors = parent_face_hmesh_ptr->get_vertices_around_face(face);
                    std::vector<vec3> face_vertex_coords_3d(face_vertex_descriptors.size());

                    for (std::vector<vd_t>::const_iterator i = face_vertex_descriptors.cbegin(); i != face_vertex_descriptors.cend(); ++i) {
                        const size_t idx = std::distance(face_vertex_descriptors.cbegin(), i);
                        const vec3& coords = parent_face_hmesh_ptr->vertex(*i);
                        face_vertex_coords_3d[idx] = coords;
                    }

                    // :::::::::::::::::::::::::
                    // project face coords to 2D
                    std::vector<vec2> face_vertex_coords_2d;

                    project_to_2d(face_vertex_coords_2d, face_vertex_coords_3d, fpi.polygon_normal, fpi.polygon_normal_largest_component);

                    const int face_edge_count = (int)face_vertex_descriptors.size(); // num edges == num verts
                    const int face_vertex_count = face_edge_count;

                    // for each edge of face
                    for (int edge_iter = 0; edge_iter < face_edge_count; ++edge_iter) {

                        const vec2& face_edge_v0 = SAFE_ACCESS(face_vertex_coords_2d, ((size_t)edge_iter) + 0);
                        const vec2& face_edge_v1 = SAFE_ACCESS(face_vertex_coords_2d, (((size_t)edge_iter) + 1) % face_vertex_count);

                        // ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
                        // Does the current edge of "face" intersect/pass through the area of
                        // the current floating polygon?

                        bool have_face_edge_intersecting_fp = false;

                        // for each edge of current floating poly
                        for (int fp_face_edge_iter = 0; fp_face_edge_iter < (int)floating_poly_edge_count; ++fp_face_edge_iter) {

                            const vec2& fp_edge_v0 = floating_poly_vertices_2d[((size_t)fp_face_edge_iter) + 0];
                            const vec2& fp_edge_v1 = floating_poly_vertices_2d[(((size_t)fp_face_edge_iter) + 1) % floating_poly_vertex_count];

                            // placeholders
                            double _1; // unused
                            double _2; // unused
                            vec2 _3; // unused

                            const char res = compute_segment_intersection(face_edge_v0, face_edge_v1, fp_edge_v0, fp_edge_v1, _3, _1, _2);

                            if (res == '1') { // implies a propery segment-segment intersection
                                have_face_edge_intersecting_fp = true;
                                break;
                            }
                        }

                        if (have_face_edge_intersecting_fp == false && face_containing_floating_poly == hmesh_t::null_face()) {
                            // here we also do a test to find if the current face actually contains
                            // the floating polygon in its area. We will need this information in order to
                            // know the correct birth-face child-face that will be further partitioned
                            // so as to prevent the current floating polygon from coming up again in the
                            // next dispatch call.

                            // for each floating polygon vertex ...
                            for (int fpVertIter = 0; fpVertIter < (int)floating_poly_vertices_2d.size(); ++fpVertIter) {
                                const char ret = compute_point_in_polygon_test(SAFE_ACCESS(floating_poly_vertices_2d, fpVertIter), face_vertex_coords_2d);
                                if (ret == 'i') { // check if strictly interior
                                    face_containing_floating_poly = *it;
                                    break;
                                }
                            }
                        }

                        if (have_face_edge_intersecting_fp) {
                            have_face_intersecting_fp = true;
                            break;
                        }
                    } // for (std::vector<hd_t>::const_iterator hIt = halfedges.cbegin(); ...

                    if (have_face_intersecting_fp) {
                        break; // done
                    }

                } // for (std::vector<fd_t>::const_iterator it = faces_from_partitioned_birth_face.cbegin(); ...

                // i.e. there exists no partitioning-edge which passes through the current floating polygon
                do_partition_current_face = (have_face_intersecting_fp == false);

                if (do_partition_current_face) {
                    // update which face we treat as "origin_face" i.e. the one that we will partition
                    MCUT_ASSERT(face_containing_floating_poly != hmesh_t::null_face());
                    origin_face = face_containing_floating_poly;
                }

            } // if (client_hmesh_birth_face_is_partitioned_atleast_once) {
            else {
                client_hmesh_birth_face = origin_face;
            }

            if (!do_partition_current_face) {
                // skip current floating polygon no need to partition "origin_face" this time
                // because an already-added edge into "parent_face_hmesh_ptr" will prevent the current
                // floating polygon from arising
                continue; // got to next floating polygon
            }

            // gather vertices of "origin_face" (descriptors and 3d coords)

            // std::vector<vd_t> originFaceVertexDescriptors = parent_face_hmesh_ptr->get_vertices_around_face(origin_face);
            std::vector<vec3> origin_face_vertices_3d;
            // get information about each edge (used by "origin_face") that needs to be split along the respective intersection point
            const std::vector<hd_t>& origin_face_halfedges = parent_face_hmesh_ptr->get_halfedges_around_face(origin_face);

            for (std::vector<hd_t>::const_iterator i = origin_face_halfedges.cbegin(); i != origin_face_halfedges.cend(); ++i) {
                const vd_t src = parent_face_hmesh_ptr->source(*i); // NOTE: we use source so that edge iterators/indices match with internal mesh storage
                origin_face_vertices_3d.push_back(parent_face_hmesh_ptr->vertex(src));
            }

            // MCUT_ASSERT(fpi.projection_component != -1); // should be defined when we identify the floating polygon in the kernel

            // project the "origin_face" to 2D
            // Since the geometry operations we are concerned about are inherently in 2d, here we project
            // our coords from 3D to 2D. We project by eliminating the component corresponding
            // to the "origin_face"'s normal vector's largest component. ("origin_face" and our
            // floating polygon have the same normal!)
            //

            std::vector<vec2> origin_face_vertices_2d;
            project_to_2d(origin_face_vertices_2d, origin_face_vertices_3d, fpi.polygon_normal, fpi.polygon_normal_largest_component);

            // ROUGH STEPS TO COMPUTE THE LINE THAT WILL BE USED TO PARTITION origin_face
            // 1. pick two edges in the floating polygon
            // 2. compute their mid-points
            // 3. construct a [segment] with these two mid-points
            // 4. if any vertex of the floating-poly is on the [line] defined by the segment OR
            //  ... if any vertex of the origin_face on the [line] defined by the segment:
            //  --> GOTO step 1 and select another pair of edges in the floating poly
            // 5. construct a ray with the segment whose origin lies outside origin_face
            // 6. intersect the ray with all edges of origin_face, and keep the intersection points [on the boundary] of origin_face
            // 7. compute mid-point of our segment (from the two mid-points in step 3)
            // 8. Get the two closest intersection points to this mid-point of our segment
            // 9. Partition origin_face using the two closest intersection points this mid-point
            // 10. Likewise update the connectivity of neighbouring faces of origin_face
            // --> Neighbours to update are inferred from the halfedges that are partitioned at the two intersection points
            // 11. remove "origin_face" from "parent_face_hmesh_ptr"
            // 12. remove neighbours of "origin_face" from "parent_face_hmesh_ptr" that shared the edge on which the two intersection points lie.
            // 13. add the child_polygons of "origin_face" and the re-traced neighbours into "parent_face_hmesh_ptr"
            // 14.  store a mapping from newly traced polygons to the original (user provided) input mesh elements
            // --> This will also be used client vertex- and face-data mapping.

            const auto fp_get_edge_vertex_coords = [&](const int fp_edge_idx, vec2& fp_edge_v0, vec2& fp_edge_v1) {
                const size_t fp_edge_v0_idx = (((size_t)fp_edge_idx) + 0);
                fp_edge_v0 = floating_poly_vertices_2d[fp_edge_v0_idx];
                const size_t fp_edge_v1_idx = (((size_t)fp_edge_idx) + 1) % floating_poly_vertex_count;
                fp_edge_v1 = floating_poly_vertices_2d[fp_edge_v1_idx];
            };

            const auto fp_get_edge_midpoint = [&](int edgeIdx) {
                vec2 edgeV0;
                vec2 edgeV1;

                fp_get_edge_vertex_coords(edgeIdx, edgeV0, edgeV1);

                const vec2 midPoint(
                    (edgeV0.x() + edgeV1.x()) / double(2.0), //
                    (edgeV0.y() + edgeV1.y()) / double(2.0));

                return midPoint;
            };

            auto fp_get_midpoint_distance = [&](std::pair<int, int> edgePair) {
                const vec2 edge0MidPoint = fp_get_edge_midpoint(edgePair.first);
                const vec2 edge1MidPoint = fp_get_edge_midpoint(edgePair.second);
                const double dist = squared_length(edge1MidPoint - edge0MidPoint);
                return dist;
            };

            // NOTE: using max (i.e. < operator) lead to floating point precision issues on
            // test 40. The only solution to which is exact arithmetic. However, since we still
            // want MCUT to work even if the user only has fixed precision numbers.
            // We pick edges based on this which are closest. No worries about colinear edges
            // because they will be detected later and skipped!
            auto fp_max_dist_predicate = [&](std::pair<int, int> edgePairA, std::pair<int, int> edgePairB) -> bool {
                const double aDist = fp_get_midpoint_distance(edgePairA);
                const double bDist = fp_get_midpoint_distance(edgePairB);
                return aDist < bDist;
            };

            std::priority_queue<
                std::pair<int, int>, //
                std::vector<std::pair<int, int>>, //
                decltype(fp_max_dist_predicate)>
                fp_edge_pair_priority_queue(fp_max_dist_predicate);

            // populate queue with [unique] pairs of edges from the floating polygon
            // priority is given to those pairs with the farthest distance between then
            for (int i = 0; i < (int)floating_poly_edge_count; ++i) {
                for (int j = i + 1; j < (int)floating_poly_edge_count; ++j) {
                    fp_edge_pair_priority_queue.push(std::make_pair(i, j));
                }
            }

            MCUT_ASSERT(fp_edge_pair_priority_queue.size() >= 3); // we can have at least 3 pairs for the simplest polygon (triangle) i.e. assuming it is not generate

            // In the next while loop, each iteration will attempt to contruct a line [passing through
            // our floating polygon] that will be used partition "origin_face" .
            // NOTE: the reason we have a while loop is because it allows us to test several possible lines
            // with-which "origin_face" can be partitioned. Some lines may not usable because they pass through
            // a vertex of the floating polygon or a vertex the "origin_face" - in which case GP will be
            // violated.
            //

            bool haveSegmentOnFP = false; // the current pair of floating polygon edges worked!

            // the line segment constructed from midpoints of two edges of the
            // floating polygon
            std::pair<vec2, vec2> fpSegment;

            while (!fp_edge_pair_priority_queue.empty() && !haveSegmentOnFP) {

                const std::pair<int, int> fpEdgePairCur = fp_edge_pair_priority_queue.top();
                fp_edge_pair_priority_queue.pop();

                const vec2 fpEdge0Midpoint = fp_get_edge_midpoint(fpEdgePairCur.first);
                const vec2 fpEdge1Midpoint = fp_get_edge_midpoint(fpEdgePairCur.second);

                // :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
                // if the line intersects/passes through a vertex in "origin_face" or a vertex in
                // the floating polygon then try another edge pair.

                auto anyPointIsOnLine = [&](
                                            const vec2& segStart,
                                            const vec2& segEnd,
                                            const std::vector<vec2>& polyVerts) -> bool {
                    double predResult(0xdeadbeef);
                    for (std::vector<vec2>::const_iterator it = polyVerts.cbegin(); it != polyVerts.cend(); ++it) {

                        bool are_collinear = collinear(segStart, segEnd, (*it), predResult);
                        // last ditch attempt to prevent the possibility of creating a partitioning
                        // edge that more-or-less passes through a vertex (of origin-face or the floatig poly itself)
                        // see: test41
                        const double epsilon = 1e-6;
                        if (are_collinear || (!are_collinear && epsilon > std::fabs(predResult))) {
                            return true;
                        }
                    }
                    return false;
                }; // end lambda

                // do we have general position? i.e. line segment does not pass through a vertex of the
                // floating polygon and "origin_face"
                bool haveGPOnFP = !anyPointIsOnLine(fpEdge0Midpoint, fpEdge1Midpoint, floating_poly_vertices_2d);
                bool haveGPOnOriginFace = !anyPointIsOnLine(fpEdge0Midpoint, fpEdge1Midpoint, origin_face_vertices_2d);
                bool haveGP = haveGPOnFP && haveGPOnOriginFace;

                if (haveGP /*|| true*/) {
                    haveSegmentOnFP = true;
                    fpSegment.first = fpEdge1Midpoint;
                    fpSegment.second = fpEdge0Midpoint;
                }

            } // while (fp_edge_pair_priority_queue.size() > 0 && successivelyPartitionedOriginFaceWithCurrentEdgePair == false) {

            if (!haveSegmentOnFP) {
                // OH OH!
                // You have encountered an extremely rare problem case.
                // Email the developers (there is a solution but it requires numerical perturbation on "fpSegment").
                throw std::logic_error("Floating-polygon partitioning step could not find a usable fpSegment");
            }

            // :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
            // At this point we have a valid line segment with which we can proceed to
            // partition the "origin_mesh".

            // Now we compute intersection points between every edge in "origin_face" and
            // our segment (treating "fpSegment" as an infinitely-long line)

            const int originFaceVertexCount = (int)origin_face_vertices_3d.size();
            MCUT_ASSERT(originFaceVertexCount >= 3);
            const int originFaceEdgeCount = originFaceVertexCount;

            // this maps stores the intersection points between our line segment and the
            // edges of "origin_face"
            std::vector<
                // information about "origin_face" edge that is to be split
                std::pair<
                    // index of an edge (i.e. index in the halfedge-list, where this list is defined w.r.t the
                    // order of halfedges_around_face(origin_face)
                    int,
                    std::pair<
                        vec2, // intersection point coords
                        double //  parameter value (t) of intersection point along our edge (used to recover 3D coords)
                        >>>
                originFaceIntersectedEdgeInfo;

            // *************************************************************************************************************
            // NOTE: "origFaceEdgeIter==0" corresponds to the second halfedge in the list returned by
            // "get_halfedges_around_face(origin_face)".
            // This is because "get_halfedges_around_face" builds the list of vertices by storing the target (not source) of
            // each halfedge of a given face.
            // *************************************************************************************************************

            // for each edge in "origin_face"
            for (int origFaceEdgeIter = 0; origFaceEdgeIter < originFaceEdgeCount; ++origFaceEdgeIter) {

                const vec2& origFaceEdgeV0 = SAFE_ACCESS(origin_face_vertices_2d, ((size_t)origFaceEdgeIter) + 0);
                const vec2& origFaceEdgeV1 = SAFE_ACCESS(origin_face_vertices_2d, ((origFaceEdgeIter) + 1) % originFaceVertexCount);

                const double garbageVal(0xdeadbeef);
                vec2 intersectionPoint(garbageVal);

                double origFaceEdgeParam;
                double fpEdgeParam;

                char intersectionResult = compute_segment_intersection(
                    origFaceEdgeV0, origFaceEdgeV1, fpSegment.first, fpSegment.second, intersectionPoint, origFaceEdgeParam, fpEdgeParam);

                // These assertion must hold since, by construction, "fpSegment" (computed from two edges
                // of the floating polygon) partitions the floating polygon which lies inside the area
                // of "origin_face".
                // Thus "fpSegment" can never intersect any half|edge/segment of "origin_face". It is the
                // infinite-line represented by the "fpSegment" that can intersect edges of "origin_face".
                MCUT_ASSERT(intersectionResult != '1'); // implies segment-segment intersection
                MCUT_ASSERT(intersectionResult != 'v'); // implies that at-least one vertex of one segment touches the other
                MCUT_ASSERT(intersectionResult != 'e'); // implies that segments collinearly overlap

                if (
                    // intersection point was successively computed i.e. the infinite-line of "fpSegment" intersected the edge of "origin_face" (somewhere including outside of "origin_face")
                    (intersectionPoint.x() != garbageVal && intersectionPoint.y() != garbageVal) &&
                    // no actual segment-segment intersection exists, which is what we want
                    intersectionResult == '0') {
                    originFaceIntersectedEdgeInfo.push_back(std::make_pair(origFaceEdgeIter, std::make_pair(intersectionPoint, origFaceEdgeParam)));
                }
            } // for (int origFaceEdgeIter = 0; origFaceEdgeIter < originFaceEdgeCount; ++origFaceEdgeIter) {

            // compute mid-point of "fpSegment", which we will used to find closest intersection points

            const vec2 fpSegmentMidPoint(
                (fpSegment.first.x() + fpSegment.second.x()) * double(0.5), //
                (fpSegment.first.y() + fpSegment.second.y()) * double(0.5));

            // Get the two closest [valid] intersection points to "fpSegmentMidPoint".
            // We do this by sorting elements of "originFaceIntersectedEdgeInfo" by the distance
            // of their respective intersection point from "fpSegmentMidPoint". We skip intersection
            // points that do not lie on an edge of "origin_face" because they introduce ambiguities
            // and that they are technically not usable (i.e. they are outside "origin_face").

            std::sort(originFaceIntersectedEdgeInfo.begin(), originFaceIntersectedEdgeInfo.end(),
                [&](const std::pair<int, std::pair<vec2, double>>& a, //
                    const std::pair<int, std::pair<vec2, double>>& b) {
                    double aDist(std::numeric_limits<double>::max()); // bias toward points inside polygon
                    // char aOnEdge = compute_point_in_polygon_test(
                    //     a.second.first,
                    //     origin_face_vertices_2d.data(),
                    //     (int)origin_face_vertices_2d.size());

                    bool aOnEdge = (double(.0) <= a.second.second && double(1.) >= a.second.second);
                    // for (int i = 0; i < (int)origin_face_vertices_2d.size(); ++i) {
                    //     int i0 = i;
                    //     int i1 = (i0 + 1) % (int)origin_face_vertices_2d.size();
                    //     if (collinear(origin_face_vertices_2d[i0], origin_face_vertices_2d[i1], a.second.first)) {
                    //          aOnEdge = true;
                    //          break;
                    //     }
                    // }

                    if (aOnEdge) {
                        const vec2 aVec = a.second.first - fpSegmentMidPoint;
                        aDist = squared_length(aVec);
                    }

                    double bDist(std::numeric_limits<double>::max());
                    // char bOnEdge = compute_point_in_polygon_test(
                    //     b.second.first,
                    //     origin_face_vertices_2d.data(),
                    //     (int)origin_face_vertices_2d.size());
                    bool bOnEdge = (double(.0) <= b.second.second && double(1.) >= b.second.second);

                    // for (int i = 0; i < (int)origin_face_vertices_2d.size(); ++i) {
                    //     int i0 = i;
                    //     int i1 = (i0 + 1) % (int)origin_face_vertices_2d.size();
                    //     if (collinear(origin_face_vertices_2d[i0], origin_face_vertices_2d[i1], b.second.first)) {
                    //         bOnEdge = true;
                    //         break;
                    //     }
                    // }

                    if (bOnEdge) {
                        const vec2 bVec = b.second.first - fpSegmentMidPoint;
                        bDist = squared_length(bVec);
                    }

                    return aDist < bDist;
                });

            // ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
            // At this point we have all information necessary to partition "origin_face" using
            // the two closest intersection points to "fpSegmentMidPoint".
            //

            // this std::vector stores the faces that use an edge that will be partitioned
            std::vector<fd_t> replaced_input_mesh_faces = { origin_face };

            MCUT_ASSERT(originFaceIntersectedEdgeInfo.size() >= 2); // we partition atleast two edges of origin_face [always!]

            // origFaceEdge0: This is the first edge in the list after sorting.
            // ---------------------------------------------------------------

            const std::pair<int, std::pair<vec2, double>>& originFaceIntersectedEdge0Info = originFaceIntersectedEdgeInfo[0]; // first elem
            const int origFaceEdge0Idx = originFaceIntersectedEdge0Info.first;
            const double& origFaceEdge0IntPointEqnParam = originFaceIntersectedEdge0Info.second.second;

            // NOTE: minus-1 since "get_vertices_around_face(origin_face)" builds a list using halfedge target vertices
            // See the starred note above
            int halfedgeIdx = origFaceEdge0Idx; // wrap_integer(origFaceEdge0Idx - 1, 0, (int)originFaceEdgeCount - 1); //(origFaceEdge0Idx + 1) % originFaceEdgeCount;
            const hd_t origFaceEdge0Halfedge = SAFE_ACCESS(origin_face_halfedges, halfedgeIdx);
            MCUT_ASSERT(origin_face == parent_face_hmesh_ptr->face(origFaceEdge0Halfedge));
            const ed_t origFaceEdge0Descr = parent_face_hmesh_ptr->edge(origFaceEdge0Halfedge);
            const vd_t origFaceEdge0HalfedgeSrcDescr = parent_face_hmesh_ptr->source(origFaceEdge0Halfedge);
            const vd_t origFaceEdge0HalfedgeTgtDescr = parent_face_hmesh_ptr->target(origFaceEdge0Halfedge);

            // query src and tgt coords and build edge vector (i.e. "tgt - src"), which is in 3d
            const vec3& origFaceEdge0HalfedgeSrc = parent_face_hmesh_ptr->vertex(origFaceEdge0HalfedgeSrcDescr);
            const vec3& origFaceEdge0HalfedgeTgt = parent_face_hmesh_ptr->vertex(origFaceEdge0HalfedgeTgtDescr);

            // infer 3D intersection point along edge using "origFaceEdge0IntPointEqnParam"
            const vec3 origFaceEdge0Vec = (origFaceEdge0HalfedgeTgt - origFaceEdge0HalfedgeSrc);
            const vec3 origFaceEdge0IntPoint3d = origFaceEdge0HalfedgeSrc + (origFaceEdge0Vec * origFaceEdge0IntPointEqnParam);
            // TODO: ensure that "origFaceEdge0IntPoint3d" lies on the plane of "origFace", this is a source of many problems"""
            const hd_t origFaceEdge0HalfedgeOpp = parent_face_hmesh_ptr->opposite(origFaceEdge0Halfedge);
            const fd_t origFaceEdge0HalfedgeOppFace = parent_face_hmesh_ptr->face(origFaceEdge0HalfedgeOpp);

            if (origFaceEdge0HalfedgeOppFace != hmesh_t::null_face()) { // exists
                // this check is needed in the case that both partitioned edges in "origin_face"
                // are incident to the same two faces
                const bool contained = std::find(replaced_input_mesh_faces.cbegin(), replaced_input_mesh_faces.cend(), origFaceEdge0HalfedgeOppFace) != replaced_input_mesh_faces.cend();
                if (!contained) {
                    replaced_input_mesh_faces.push_back(origFaceEdge0HalfedgeOppFace);
                }
            }

            // origFaceEdge1: This is the second edge in the list after sorting.
            // ---------------------------------------------------------------

            const std::pair<int, std::pair<vec2, double>>& originFaceIntersectedEdge1Info = originFaceIntersectedEdgeInfo[1]; // second elem
            const int origFaceEdge1Idx = originFaceIntersectedEdge1Info.first;
            const double& origFaceEdge1IntPointEqnParam = originFaceIntersectedEdge1Info.second.second;

            halfedgeIdx = origFaceEdge1Idx; /// wrap_integer(origFaceEdge1Idx - 1, 0, (int)originFaceEdgeCount - 1); // (origFaceEdge1Idx + 1) % originFaceEdgeCount;
            const hd_t origFaceEdge1Halfedge = SAFE_ACCESS(origin_face_halfedges, halfedgeIdx);
            MCUT_ASSERT(origin_face == parent_face_hmesh_ptr->face(origFaceEdge1Halfedge));
            const ed_t origFaceEdge1Descr = parent_face_hmesh_ptr->edge(origFaceEdge1Halfedge);
            const vd_t origFaceEdge1HalfedgeSrcDescr = parent_face_hmesh_ptr->source(origFaceEdge1Halfedge);
            const vd_t origFaceEdge1HalfedgeTgtDescr = parent_face_hmesh_ptr->target(origFaceEdge1Halfedge);

            // query src and tgt positions and build vector tgt - src
            const vec3& origFaceEdge1HalfedgeSrc = parent_face_hmesh_ptr->vertex(origFaceEdge1HalfedgeSrcDescr);
            const vec3& origFaceEdge1HalfedgeTgt = parent_face_hmesh_ptr->vertex(origFaceEdge1HalfedgeTgtDescr);

            // infer intersection point in 3d using "origFaceEdge0IntPointEqnParam"
            const vec3 origFaceEdge1Vec = (origFaceEdge1HalfedgeTgt - origFaceEdge1HalfedgeSrc);
            const vec3 origFaceEdge1IntPoint3d = origFaceEdge1HalfedgeSrc + (origFaceEdge1Vec * origFaceEdge1IntPointEqnParam);

            const hd_t origFaceEdge1HalfedgeOpp = parent_face_hmesh_ptr->opposite(origFaceEdge1Halfedge);
            const fd_t origFaceEdge1HalfedgeOppFace = parent_face_hmesh_ptr->face(origFaceEdge1HalfedgeOpp);

            if (origFaceEdge1HalfedgeOppFace != hmesh_t::null_face()) { // exists
                const bool contained = std::find(replaced_input_mesh_faces.cbegin(), replaced_input_mesh_faces.cend(), origFaceEdge1HalfedgeOppFace) != replaced_input_mesh_faces.cend();
                if (!contained) {
                    replaced_input_mesh_faces.push_back(origFaceEdge1HalfedgeOppFace);
                }
            }

            // gather halfedges of each neighbouring face of "origin_face" that is to be replaced
            std::unordered_map<fd_t, std::vector<hd_t>> replacedOrigFaceNeighbourToOldHalfedges;

            for (std::vector<fd_t>::const_iterator it = replaced_input_mesh_faces.cbegin(); it != replaced_input_mesh_faces.cend(); ++it) {
                if (*it == origin_face) {
                    continue;
                }
                replacedOrigFaceNeighbourToOldHalfedges[*it] = parent_face_hmesh_ptr->get_halfedges_around_face(*it);
            }

            // :::::::::::::::::::::::::::::::::::::::::::::::::::::
            //** add new intersection points into parent_face_hmesh_ptr

            const vd_t origFaceEdge0IntPoint3dDescr = parent_face_hmesh_ptr->add_vertex(origFaceEdge0IntPoint3d);
            MCUT_ASSERT(new_poly_partition_vertices.count(origFaceEdge0IntPoint3dDescr) == 0);
            new_poly_partition_vertices[origFaceEdge0IntPoint3dDescr] = origFaceEdge0IntPoint3d;

            const vd_t origFaceEdge1IntPoint3dDescr = parent_face_hmesh_ptr->add_vertex(origFaceEdge1IntPoint3d);
            MCUT_ASSERT(new_poly_partition_vertices.count(origFaceEdge1IntPoint3dDescr) == 0);
            new_poly_partition_vertices[origFaceEdge1IntPoint3dDescr] = origFaceEdge1IntPoint3d;

            // :::::::::::
            //** add edges

            // halfedge between the intersection points
            const hd_t intPointHalfedgeDescr = parent_face_hmesh_ptr->add_edge(origFaceEdge0IntPoint3dDescr, origFaceEdge1IntPoint3dDescr);

            // partitioning edges for origFaceEdge0
            const hd_t origFaceEdge0FirstNewHalfedgeDescr = parent_face_hmesh_ptr->add_edge(origFaceEdge0HalfedgeSrcDescr, origFaceEdge0IntPoint3dDescr); // o --> x
            const hd_t origFaceEdge0SecondNewHalfedgeDescr = parent_face_hmesh_ptr->add_edge(origFaceEdge0IntPoint3dDescr, origFaceEdge0HalfedgeTgtDescr); // x --> o

            // partitioning edges for origFaceEdge1
            const hd_t origFaceEdge1FirstNewHalfedgeDescr = parent_face_hmesh_ptr->add_edge(origFaceEdge1HalfedgeSrcDescr, origFaceEdge1IntPoint3dDescr); // o--> x
            const hd_t origFaceEdge1SecondNewHalfedgeDescr = parent_face_hmesh_ptr->add_edge(origFaceEdge1IntPoint3dDescr, origFaceEdge1HalfedgeTgtDescr); // x --> o

            // We will now re-trace the face that are incident to the partitioned edges to create
            // new faces.
            std::unordered_map<fd_t, std::vector<hd_t>> replacedOrigFaceNeighbourToNewHalfedges;

            // NOTE: first we retrace the neighbouring polygons that shared a partitioned edge with "origin_face".
            // These are somewhat easier to deal with first because a fixed set of steps can be followed with a simple for-loop.

            // for each neighbouring face (w.r.t. "origin_face") to be replaced
            for (std::unordered_map<fd_t, std::vector<hd_t>>::const_iterator i = replacedOrigFaceNeighbourToOldHalfedges.cbegin();
                 i != replacedOrigFaceNeighbourToOldHalfedges.cend();
                 ++i) {

                fd_t face = i->first;
                MCUT_ASSERT(face != origin_face); // avoid complex case here, where we need to partition the polygon in two. We'll handle that later.

                const std::vector<hd_t>& oldHalfedges = i->second;

                // for each halfedge of face
                for (std::vector<hd_t>::const_iterator j = oldHalfedges.cbegin(); j != oldHalfedges.cend(); ++j) {

                    const hd_t oldHalfedge = *j;
                    hd_t newHalfedge = hmesh_t::null_halfedge();
                    const ed_t oldHalfedgeEdge = parent_face_hmesh_ptr->edge(oldHalfedge);

                    // is the halfedge part of an edge that is to be partitioned...?

                    if (oldHalfedgeEdge == origFaceEdge0Descr) {
                        hd_t firstNewHalfedge = parent_face_hmesh_ptr->opposite(origFaceEdge0SecondNewHalfedgeDescr);
                        replacedOrigFaceNeighbourToNewHalfedges[face].push_back(firstNewHalfedge);
                        hd_t secondNewHalfedge = parent_face_hmesh_ptr->opposite(origFaceEdge0FirstNewHalfedgeDescr);
                        replacedOrigFaceNeighbourToNewHalfedges[face].push_back(secondNewHalfedge);
                    } else if (oldHalfedgeEdge == origFaceEdge1Descr) {
                        hd_t firstNewHalfedge = parent_face_hmesh_ptr->opposite(origFaceEdge1SecondNewHalfedgeDescr);
                        replacedOrigFaceNeighbourToNewHalfedges[face].push_back(firstNewHalfedge);
                        hd_t secondNewHalfedge = parent_face_hmesh_ptr->opposite(origFaceEdge1FirstNewHalfedgeDescr);
                        replacedOrigFaceNeighbourToNewHalfedges[face].push_back(secondNewHalfedge);
                    } else {
                        replacedOrigFaceNeighbourToNewHalfedges[face].push_back(oldHalfedge); // maintain unpartitioned halfedge
                    }
                } // for (std::vector<hd_t>::const_iterator j = oldHalfedges.cbegin(); j != oldHalfedges.cend(); ++j) {

                // remove neighbour face
                parent_face_hmesh_ptr->remove_face(i->first);

                // immediately add the updated tracing of the neighbour face so that it maintains the same desciptor!
                std::vector<vd_t> faceVertices;
                for (std::vector<hd_t>::const_iterator it = replacedOrigFaceNeighbourToNewHalfedges[face].cbegin();
                     it != replacedOrigFaceNeighbourToNewHalfedges[face].cend(); ++it) {
                    const vd_t tgt = parent_face_hmesh_ptr->target(*it);
                    faceVertices.push_back(tgt);
                }

                const fd_t fdescr = parent_face_hmesh_ptr->add_face(faceVertices);
                MCUT_ASSERT(fdescr == i->first);

#if 0
                        std::unordered_map<fd_t, fd_t>::const_iterator fiter = child_to_client_birth_face.find(fdescr);

                        bool descrIsMapped = (fiter != child_to_client_birth_face.cend());

                        if (!descrIsMapped) {
                            child_to_client_birth_face[fdescr] = client_hmesh_birth_face;
                        }
#endif
            } // for (std::unordered_map<fd_t, std::vector<hd_t>>::const_iterator i = replacedOrigFaceNeighbourToOldHalfedges.cbegin(); i != replacedOrigFaceNeighbourToOldHalfedges.cend(); ++i) {

            // ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
            // Here we now handle the complex case where we need to partition
            // "origin_face" in two new faces.

            parent_face_hmesh_ptr->remove_face(origin_face); // one free slot

            // This queue contains the halfegdes that we'll start to trace our new faces from
            // (those connected to our new intersection points)
            std::queue<hd_t> origFaceiHalfedges;
            origFaceiHalfedges.push(intPointHalfedgeDescr);
            origFaceiHalfedges.push(parent_face_hmesh_ptr->opposite(intPointHalfedgeDescr));

            // this list containing all halfedges along the boundary of "origin_face"
            std::vector<hd_t> origFaceBoundaryHalfdges = { // first add the new boundary-edge partitioning halfedges, since we already know them
                origFaceEdge0FirstNewHalfedgeDescr,
                origFaceEdge0SecondNewHalfedgeDescr,
                origFaceEdge1FirstNewHalfedgeDescr,
                origFaceEdge1SecondNewHalfedgeDescr
            };

            // .... now we add the remaining boundary halfedges of "origin_face" i.e. those not partitioneds
            for (std::vector<hd_t>::const_iterator it = origin_face_halfedges.cbegin(); it != origin_face_halfedges.cend(); ++it) {
                if (*it != origFaceEdge0Halfedge && *it != origFaceEdge1Halfedge) { // if its not one of the replaced/partitioned halfedges
                    origFaceBoundaryHalfdges.push_back(*it);
                }
            }

            // here we will store the tracing of the two child polygons that result from partitioning "origin_face"
            std::vector<std::vector<hd_t>> origFaceChildPolygons;

            do { // each iteration will trace a child polygon
                hd_t childPolyHE_cur = hmesh_t::null_halfedge();
                hd_t childPolyHE_next = origFaceiHalfedges.front(); // start
                origFaceiHalfedges.pop();

                origFaceChildPolygons.push_back(std::vector<hd_t>());
                std::vector<hd_t>& origFaceChildPoly = origFaceChildPolygons.back();

                const hd_t firstHalfedge = childPolyHE_next;
                const vd_t firstHalfedgeSrc = parent_face_hmesh_ptr->source(firstHalfedge);

                do {
                    childPolyHE_cur = childPolyHE_next;
                    origFaceChildPoly.push_back(childPolyHE_cur);
                    const vd_t childPolyHE_curTgt = parent_face_hmesh_ptr->target(childPolyHE_cur);
                    childPolyHE_cur = hmesh_t::null_halfedge();
                    childPolyHE_next = hmesh_t::null_halfedge();

                    if (childPolyHE_curTgt != firstHalfedgeSrc) {
                        // find next halfedge to continue building the current child polygon
                        std::vector<hd_t>::const_iterator fiter = std::find_if(origFaceBoundaryHalfdges.cbegin(), origFaceBoundaryHalfdges.cend(),
                            [&](const hd_t h) { // find a boundary halfedge that can be connected to the current halfedge
                                const vd_t src = parent_face_hmesh_ptr->source(h);
                                return src == childPolyHE_curTgt;
                            });

                        MCUT_ASSERT(fiter != origFaceBoundaryHalfdges.cend());

                        childPolyHE_next = *fiter;
                    }

                } while (childPolyHE_next != hmesh_t::null_halfedge());

                MCUT_ASSERT(origFaceChildPoly.size() >= 3); // minimum size of valid polygon (triangle)

                // Add child face into mesh
                std::vector<vd_t> origFaceChildPolyVertices;

                for (std::vector<hd_t>::const_iterator hIt = origFaceChildPoly.cbegin(); hIt != origFaceChildPoly.cend(); ++hIt) {
                    const vd_t tgt = parent_face_hmesh_ptr->target(*hIt);
                    origFaceChildPolyVertices.push_back(tgt);
                }

                const fd_t fdescr = parent_face_hmesh_ptr->add_face(origFaceChildPolyVertices);
                MCUT_ASSERT(fdescr != hmesh_t::null_face());

                if (origFaceChildPolygons.size() == 1) {
                    // the first child face will re-use the descriptor of "origin_face".
                    MCUT_ASSERT(fdescr == origin_face);
                }

                child_to_client_birth_face[fdescr] = client_hmesh_birth_face;

            } while (origFaceiHalfedges.empty() == false);

            MCUT_ASSERT(origFaceChildPolygons.size() == 2); // "origin_face" shall only ever be partition into two child polygons

            // remove the partitioned/'splitted' edges
            parent_face_hmesh_ptr->remove_edge(origFaceEdge0Descr);
            parent_face_hmesh_ptr->remove_edge(origFaceEdge1Descr);

        } // for (std::vector<floating_polygon_info_t>::const_iterator floating_poly_info_iter = detected_floating_polygons_iter->second.cbegin(); ...
    } // for (std::vector<floating_polygon_info_t>::const_iterator detected_floating_polygons_iter = kernel_output.detected_floating_polygons.cbegin(); ...
}

extern "C" void preproc(
    std::unique_ptr<context_t>& context_uptr,
    const void* pSrcMeshVertices,
    const uint32_t* pSrcMeshFaceIndices,
    const uint32_t* pSrcMeshFaceSizes,
    uint32_t numSrcMeshVertices,
    uint32_t numSrcMeshFaces,
    const void* pCutMeshVertices,
    const uint32_t* pCutMeshFaceIndices,
    const uint32_t* pCutMeshFaceSizes,
    uint32_t numCutMeshVertices,
    uint32_t numCutMeshFaces) noexcept(false)
{
    hmesh_t source_hmesh;
    double source_hmesh_aabb_diag(0.0);

    if (false == client_input_arrays_to_hmesh(context_uptr, source_hmesh, source_hmesh_aabb_diag, pSrcMeshVertices, pSrcMeshFaceIndices, pSrcMeshFaceSizes, numSrcMeshVertices, numSrcMeshFaces)) {
        throw std::invalid_argument("invalid source-mesh arrays");
    }

    if (false == check_input_mesh(context_uptr, source_hmesh)) {
        throw std::invalid_argument("invalid source-mesh connectivity");
    }

    input_t kernel_input; // kernel/backend inpout

#if defined(MCUT_MULTI_THREADED)
    kernel_input.scheduler = &context_uptr->scheduler;
#endif

    kernel_input.src_mesh = &source_hmesh;

    kernel_input.verbose = false;
    kernel_input.require_looped_cutpaths = false;

    kernel_input.verbose = static_cast<bool>((context_uptr->flags & MC_DEBUG) && (context_uptr->debugType & MC_DEBUG_SOURCE_KERNEL));
    kernel_input.require_looped_cutpaths = static_cast<bool>(context_uptr->dispatchFlags & MC_DISPATCH_REQUIRE_THROUGH_CUTS);
    kernel_input.populate_vertex_maps = static_cast<bool>(context_uptr->dispatchFlags & MC_DISPATCH_INCLUDE_VERTEX_MAP);
    kernel_input.populate_face_maps = static_cast<bool>(context_uptr->dispatchFlags & MC_DISPATCH_INCLUDE_FACE_MAP);

    uint32_t dispatch_filter_flag_bitset_all = ( //
        MC_DISPATCH_FILTER_FRAGMENT_LOCATION_ABOVE | //
        MC_DISPATCH_FILTER_FRAGMENT_LOCATION_BELOW | //
        MC_DISPATCH_FILTER_FRAGMENT_LOCATION_UNDEFINED | //
        MC_DISPATCH_FILTER_FRAGMENT_SEALING_INSIDE | //
        MC_DISPATCH_FILTER_FRAGMENT_SEALING_OUTSIDE | //
        MC_DISPATCH_FILTER_FRAGMENT_SEALING_NONE | //
        MC_DISPATCH_FILTER_PATCH_INSIDE | //
        MC_DISPATCH_FILTER_PATCH_OUTSIDE | //
        MC_DISPATCH_FILTER_SEAM_SRCMESH | //
        MC_DISPATCH_FILTER_SEAM_CUTMESH);

    const bool dispatchFilteringEnabled = static_cast<bool>(context_uptr->dispatchFlags & dispatch_filter_flag_bitset_all); // any

    if (dispatchFilteringEnabled) { // user only wants [some] output connected components
        kernel_input.keep_fragments_below_cutmesh = static_cast<bool>(context_uptr->dispatchFlags & MC_DISPATCH_FILTER_FRAGMENT_LOCATION_BELOW);
        kernel_input.keep_fragments_above_cutmesh = static_cast<bool>(context_uptr->dispatchFlags & MC_DISPATCH_FILTER_FRAGMENT_LOCATION_ABOVE);
        kernel_input.keep_fragments_sealed_outside = static_cast<bool>(context_uptr->dispatchFlags & MC_DISPATCH_FILTER_FRAGMENT_SEALING_OUTSIDE);
        kernel_input.keep_fragments_sealed_inside = static_cast<bool>(context_uptr->dispatchFlags & MC_DISPATCH_FILTER_FRAGMENT_SEALING_INSIDE);
        kernel_input.keep_unsealed_fragments = static_cast<bool>(context_uptr->dispatchFlags & MC_DISPATCH_FILTER_FRAGMENT_SEALING_NONE);
        kernel_input.keep_fragments_partially_cut = static_cast<bool>(context_uptr->dispatchFlags & MC_DISPATCH_FILTER_FRAGMENT_LOCATION_UNDEFINED);
        kernel_input.keep_inside_patches = static_cast<bool>(context_uptr->dispatchFlags & MC_DISPATCH_FILTER_PATCH_INSIDE);
        kernel_input.keep_outside_patches = static_cast<bool>(context_uptr->dispatchFlags & MC_DISPATCH_FILTER_PATCH_OUTSIDE);
        kernel_input.keep_srcmesh_seam = static_cast<bool>(context_uptr->dispatchFlags & MC_DISPATCH_FILTER_SEAM_SRCMESH);
        kernel_input.keep_cutmesh_seam = static_cast<bool>(context_uptr->dispatchFlags & MC_DISPATCH_FILTER_SEAM_CUTMESH);
    } else { // compute all possible types of connected components
        kernel_input.keep_fragments_below_cutmesh = true;
        kernel_input.keep_fragments_above_cutmesh = true;
        kernel_input.keep_fragments_partially_cut = true;
        kernel_input.keep_unsealed_fragments = true;
        kernel_input.keep_fragments_sealed_outside = true; // mutually exclusive with exhaustive case
        kernel_input.keep_fragments_sealed_inside = true;
        kernel_input.keep_fragments_sealed_outside_exhaustive = false;
        kernel_input.keep_fragments_sealed_inside_exhaustive = false;
        kernel_input.keep_inside_patches = true;
        kernel_input.keep_outside_patches = true;
        kernel_input.keep_srcmesh_seam = true;
        kernel_input.keep_cutmesh_seam = true;
    }

    kernel_input.enforce_general_position = (0 != (context_uptr->dispatchFlags & MC_DISPATCH_ENFORCE_GENERAL_POSITION));

    // Construct BVHs
    // ::::::::::::::

    context_uptr->log(MC_DEBUG_SOURCE_API, MC_DEBUG_TYPE_OTHER, 0, MC_DEBUG_SEVERITY_NOTIFICATION, "Build source-mesh BVH");

#if defined(USE_OIBVH)
    std::vector<bounding_box_t<vec3>> source_hmesh_BVH_aabb_array;
    std::vector<fd_t> source_hmesh_BVH_leafdata_array;
    std::vector<bounding_box_t<vec3>> source_hmesh_face_aabb_array;
    build_oibvh(source_hmesh, source_hmesh_BVH_aabb_array, source_hmesh_BVH_leafdata_array, source_hmesh_face_aabb_array);
#else
    BoundingVolumeHierarchy source_hmesh_BVH;
    source_hmesh_BVH.buildTree(source_hmesh);
#endif
    context_uptr->log(MC_DEBUG_SOURCE_API, MC_DEBUG_TYPE_OTHER, 0, MC_DEBUG_SEVERITY_NOTIFICATION, "Build cut-mesh BVH");

    /*
        NOTE: All variables declared as shared pointers here represent variables that live (on the heap)
        until all connected components (that are created during the current mcDispatch call) are destroyed.
        Thus, each such connected component will maintain its own (reference counted) pointer.

        These variables are used when populating client output arrays during �mcGetConnectedComponentData�.
        One example of this usage is when the client requests a connected component�s face map. In the cases
        where polygon partitioning occurs during the respective mcDispatch call then some of these shared_ptrs like
        �source_hmesh_child_to_usermesh_birth_face� will be used to generate the correct mapping from
        the faces of the connected component  to the
        client mesh (which will be internally modified due to polygon partitioning). Here the client mesh
        corresponds to the input source mesh if the connected component is a fragment, and the cut mesh otherwise.
    */

    // mapping variables from a child face to the parent face in the corresponding input-hmesh face.
    // This child face is produced as a result of polygon partition.
    std::shared_ptr< //
        std::unordered_map< //
            fd_t /*child face*/,
            fd_t /*parent face in the [user-provided] source mesh*/
            > //
        >
        source_hmesh_child_to_usermesh_birth_face = std::shared_ptr<std::unordered_map<fd_t, fd_t>>(new std::unordered_map<fd_t, fd_t>);
    
        std::unordered_map< //
            fd_t /*child face*/,
            fd_t /*parent face in the [user-provided] cut mesh*/
            >
        cut_hmesh_child_to_usermesh_birth_face ;
    // descriptors and coordinates of new vertices that are added into an input mesh (source mesh or cut mesh)
    // in order to carry out partitioning
    std::shared_ptr<std::unordered_map<vd_t, vec3>> source_hmesh_new_poly_partition_vertices = std::shared_ptr<std::unordered_map<vd_t, vec3>>(new std::unordered_map<vd_t, vec3>);
    std::unordered_map<vd_t, vec3> cut_hmesh_new_poly_partition_vertices;

    // the number of faces in the source mesh from the last/previous dispatch call
    const uint32_t source_hmesh_face_count = numSrcMeshFaces;
    uint32_t source_hmesh_face_count_prev = source_hmesh_face_count;

    output_t kernel_output;

    hmesh_t cut_hmesh; // halfedge representation of the cut-mesh
    double cut_hmesh_aabb_diag(0.0);

#if defined(USE_OIBVH)
    std::vector<bounding_box_t<vec3>> cut_hmesh_BVH_aabb_array;
    std::vector<fd_t> cut_hmesh_BVH_leafdata_array;
    std::vector<bounding_box_t<vec3>> cut_hmesh_face_face_aabb_array;
#else
    BoundingVolumeHierarchy cut_hmesh_BVH; // built later (see below)
#endif

    bool source_or_cut_hmesh_BVH_rebuilt = true; // i.e. used to determine whether we should retraverse BVHs

    std::map<fd_t, std::vector<fd_t>> ps_face_to_potentially_intersecting_others; // result of BVH traversal

#if defined(MCUT_MULTI_THREADED)
    kernel_output.status.store(status_t::SUCCESS);
#else
    kernel_output.status = status_t::SUCCESS;
#endif

    int cut_mesh_perturbation_count = 0; // number of times we have perturbed the cut mesh
    int kernel_invocation_counter = -1; // number of times we have called the internal dispatch/intersect function
    double numerical_perturbation_constant = 0.0; // = cut_hmesh_aabb_diag * GENERAL_POSITION_ENFORCMENT_CONSTANT;

    // RESOLVE mesh intersections
    // ::::::::::::::::::::::::::

    // The following loop-body contains code to do the cutting. The logic resides in a loop
    // for 2 reasons:
    // 1)   the input meshes may not be in "general position"
    // 2)   the resulting intersection between the input meshes may
    //      produce "floating polygons" (an input mesh intersects a face of the the other in such a way that none of the edges of this face are severed).
    //
    // For each reason, we need to modify the input(s) in order to have a valid (proper intersection-permitting) configuration.
    // If general position is violated, then we apply numerical perturbation of the cut-mesh.
    // And if floating polygons arise, then we partition the suspected face into two new faces with an edge that is guaranteed to be
    // severed during the cut.
    do {
        kernel_invocation_counter++;

        // here we check the reason (if any) for entering the loop body.
        // NOTE: the aforementioned 2 reasons could both be false, which will
        // be the case during the first iteration (i.e. when "kernel_invocation_counter == 0")
#if defined(MCUT_MULTI_THREADED)
        bool general_position_assumption_was_violated = ((kernel_output.status.load() == status_t::GENERAL_POSITION_VIOLATION));
        bool floating_polygon_was_detected = kernel_output.status.load() == status_t::DETECTED_FLOATING_POLYGON;
#else
        bool general_position_assumption_was_violated = (kernel_output.status == status_t::GENERAL_POSITION_VIOLATION);
        bool floating_polygon_was_detected = kernel_output.status == status_t::DETECTED_FLOATING_POLYGON;
#endif

        // Here we reset the kernel execution status
#if defined(MCUT_MULTI_THREADED)
        kernel_output.status.store(status_t::SUCCESS);
#else
        kernel_output.status = status_t::SUCCESS;
#endif

        // the (translation) vector to hold the values with which we will
        // carry out numerical perturbation of the cutting surface
        vec3 perturbation(0.0, 0.0, 0.0);

        if (general_position_assumption_was_violated) { // i.e. do we need to perturb the cut-mesh?

            MCUT_ASSERT(floating_polygon_was_detected == false); // cannot occur at same time! (see kernel)

            if (cut_mesh_perturbation_count == MAX_PERTUBATION_ATTEMPTS) {

                context_uptr->log(MC_DEBUG_SOURCE_KERNEL, MC_DEBUG_TYPE_OTHER, 0, MC_DEBUG_SEVERITY_MEDIUM, kernel_output.logger.get_reason_for_failure());

                throw std::runtime_error("max perturbation iteratons reached");
            }

            // used by the kernel track if the most-recent perturbation causes the cut-mesh and src-mesh to
            // not intersect at all, which means we need to perturb again.
            kernel_input.general_position_enforcement_count = cut_mesh_perturbation_count;

            MCUT_ASSERT(numerical_perturbation_constant != double(0.0));

            static thread_local std::default_random_engine random_engine(1);
            static thread_local std::mt19937 mersenne_twister_generator(random_engine());
            static thread_local std::uniform_real_distribution<double> uniform_distribution(-1.0, 1.0);

            for (int i = 0; i < 3; ++i) {
                perturbation[i] = uniform_distribution(mersenne_twister_generator) * numerical_perturbation_constant;
            }

            cut_mesh_perturbation_count++;
        } // if (general_position_assumption_was_violated) {

        if ((cut_mesh_perturbation_count == 0 /*no perturbs required*/ || general_position_assumption_was_violated) && floating_polygon_was_detected == false) {

            // TODO: assume that re-adding elements (vertices and faces) is going to change the order
            // from the user-provided order. So we still need to fix the mapping, which may no longer
            // be one-to-one as in the case when things do not change.
            cut_hmesh.reset();

            // TODO: the number of cut-mesh faces and vertices may increase due to polygon partitioning
            // Therefore: we need to perturb [the updated cut-mesh] i.e. the one containing partitioned polygons
            // "pCutMeshFaces" are simply the user provided faces
            // We must also use the newly added vertices (coords) due to polygon partitioning as "unperturbed" values
            // This will require some intricate mapping
            if (false == client_input_arrays_to_hmesh(context_uptr, cut_hmesh, cut_hmesh_aabb_diag, pCutMeshVertices, pCutMeshFaceIndices, pCutMeshFaceSizes, numCutMeshVertices, numCutMeshFaces, ((cut_mesh_perturbation_count == 0) ? NULL : &perturbation))) {
                throw std::invalid_argument("invalid cut-mesh arrays");
            }

            numerical_perturbation_constant = cut_hmesh_aabb_diag * GENERAL_POSITION_ENFORCMENT_CONSTANT;

            kernel_input.cut_mesh = &cut_hmesh;

            if (cut_mesh_perturbation_count == 0) { // i.e. first time we are invoking kernel intersect function
#if defined(USE_OIBVH)
                cut_hmesh_BVH_aabb_array.clear();
                cut_hmesh_BVH_leafdata_array.clear();
                build_oibvh(cut_hmesh, cut_hmesh_BVH_aabb_array, cut_hmesh_BVH_leafdata_array, cut_hmesh_face_face_aabb_array, numerical_perturbation_constant);
#else
                cut_hmesh_BVH.buildTree(cut_hmesh, numerical_perturbation_constant);
#endif
                source_or_cut_hmesh_BVH_rebuilt = true;
            }
        }

        TIMESTACK_PUSH("partition floating polygons");
        if (floating_polygon_was_detected) {

            MCUT_ASSERT(general_position_assumption_was_violated == false); // cannot occur at same time (GP violation is detected before FPs)!

            // indicates whether a polygon was partitioned on the source mesh
            bool source_hmesh_modified = false;
            // indicates whether a polygon was partitioned on the cut mesh
            bool cut_hmesh_modified = false;

            resolve_floating_polygons(
                source_hmesh_modified,
                cut_hmesh_modified,
                kernel_output.detected_floating_polygons,
                source_hmesh_face_count_prev,
                source_hmesh,
                cut_hmesh,
                source_hmesh_child_to_usermesh_birth_face.get()[0],
                cut_hmesh_child_to_usermesh_birth_face,
                source_hmesh_new_poly_partition_vertices.get()[0],
                cut_hmesh_new_poly_partition_vertices);

            // ::::::::::::::::::::::::::::::::::::::::::::
            // rebuild the BVH of "parent_face_hmesh_ptr" again

            if (source_hmesh_modified) {
#if defined(USE_OIBVH)
                source_hmesh_BVH_aabb_array.clear();
                source_hmesh_BVH_leafdata_array.clear();
                build_oibvh(
                    source_hmesh,
                    source_hmesh_BVH_aabb_array,
                    source_hmesh_BVH_leafdata_array,
                    source_hmesh_face_aabb_array);
#else
                source_hmesh_BVH.buildTree(source_hmesh);
#endif
            }

            if (cut_hmesh_modified) {
#if defined(USE_OIBVH)
                cut_hmesh_BVH_aabb_array.clear();
                cut_hmesh_BVH_leafdata_array.clear();
                build_oibvh(
                    cut_hmesh,
                    cut_hmesh_BVH_aabb_array,
                    cut_hmesh_BVH_leafdata_array,
                    cut_hmesh_face_face_aabb_array,
                    numerical_perturbation_constant);
#else
                cut_hmesh_BVH.buildTree(cut_hmesh, numerical_perturbation_constant);
#endif
            }

            source_or_cut_hmesh_BVH_rebuilt = source_hmesh_modified || cut_hmesh_modified;

            MCUT_ASSERT(source_or_cut_hmesh_BVH_rebuilt == true);

            kernel_output.detected_floating_polygons.clear();
        } // if (floating_polygon_was_detected) {
        TIMESTACK_POP();

        // Check for mesh defects
        // ::::::::::::::::::::::

        // NOTE: we check for defects here since both input meshes may be modified by the polygon partitioning process above.
        // Partitiining is involked after atleast one dispatch call.
        context_uptr->log(MC_DEBUG_SOURCE_API, MC_DEBUG_TYPE_OTHER, 0, MC_DEBUG_SEVERITY_NOTIFICATION, "Check source-mesh for defects");

        if (false == check_input_mesh(context_uptr, source_hmesh)) {
            throw std::invalid_argument("invalid source-mesh connectivity");
        }

        context_uptr->log(MC_DEBUG_SOURCE_API, MC_DEBUG_TYPE_OTHER, 0, MC_DEBUG_SEVERITY_NOTIFICATION, "Check cut-mesh for defects");

        if (false == check_input_mesh(context_uptr, cut_hmesh)) {
            throw std::invalid_argument("invalid cut-mesh connectivity");
        }

        if (source_or_cut_hmesh_BVH_rebuilt) {
            // Evaluate BVHs to find polygon pairs that will be tested for intersection
            // ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
            source_or_cut_hmesh_BVH_rebuilt = false;
            context_uptr->log(MC_DEBUG_SOURCE_API, MC_DEBUG_TYPE_OTHER, 0, MC_DEBUG_SEVERITY_NOTIFICATION, "Find potentially-intersecting polygons");

            ps_face_to_potentially_intersecting_others.clear();
#if defined(USE_OIBVH)
            intersectOIBVHs(ps_face_to_potentially_intersecting_others, source_hmesh_BVH_aabb_array, source_hmesh_BVH_leafdata_array, cut_hmesh_BVH_aabb_array, cut_hmesh_BVH_leafdata_array);
#else
            BoundingVolumeHierarchy::intersectBVHTrees(
#if defined(MCUT_MULTI_THREADED)
                context_uptr->scheduler,
#endif
                ps_face_to_potentially_intersecting_others,
                source_hmesh_BVH,
                cut_hmesh_BVH,
                0,
                source_hmesh.number_of_faces());

#endif

            context_uptr->log(
                MC_DEBUG_SOURCE_API,
                MC_DEBUG_TYPE_OTHER,
                0,
                MC_DEBUG_SEVERITY_NOTIFICATION,
                "Polygon-pairs found = " + std::to_string(ps_face_to_potentially_intersecting_others.size()));

            if (ps_face_to_potentially_intersecting_others.empty()) {
                if (general_position_assumption_was_violated && cut_mesh_perturbation_count > 0) {
                    // perturbation lead to an intersection-free state at the BVH level (and of-course the polygon level).
                    // We need to perturb again. (The whole cut mesh)
#if defined(MCUT_MULTI_THREADED)
                    kernel_output.status.store(status_t::GENERAL_POSITION_VIOLATION);
#else
                    kernel_output.status = status_t::GENERAL_POSITION_VIOLATION;
#endif
                    continue;
                } else {
                    context_uptr->log(MC_DEBUG_SOURCE_API, MC_DEBUG_TYPE_OTHER, 0, MC_DEBUG_SEVERITY_NOTIFICATION, "Mesh BVHs do not overlap.");
                    return; // we are done
                }
            }
        }

        kernel_input.ps_face_to_potentially_intersecting_others = &ps_face_to_potentially_intersecting_others;

#if defined(USE_OIBVH)
        kernel_input.source_hmesh_face_aabb_array_ptr = &source_hmesh_face_aabb_array;
        kernel_input.cut_hmesh_face_aabb_array_ptr = &cut_hmesh_face_face_aabb_array;
#else
        kernel_input.source_hmesh_BVH = &source_hmesh_BVH;
        kernel_input.cut_hmesh_BVH = &cut_hmesh_BVH;
#endif
        // Invokee the kernel by calling the internal dispatch function
        // ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

        source_hmesh_face_count_prev = source_hmesh.number_of_faces();

        try {
            context_uptr->log(MC_DEBUG_SOURCE_KERNEL, MC_DEBUG_TYPE_OTHER, 0, MC_DEBUG_SEVERITY_NOTIFICATION, "dispatch kernel");
            dispatch(kernel_output, kernel_input);
        } catch (const std::exception& e) {
            fprintf(stderr, "fatal kernel exception caught : %s\n", e.what());
            throw e;
        }
    } while (
#if defined(MCUT_MULTI_THREADED)
        (kernel_output.status.load() == status_t::GENERAL_POSITION_VIOLATION && kernel_input.enforce_general_position) || //
        kernel_output.status.load() == status_t::DETECTED_FLOATING_POLYGON
#else
        // general position voliation
        (kernel_output.status == status_t::GENERAL_POSITION_VIOLATION && kernel_input.enforce_general_position) || //
        // kernel detected a floating polygon and we now need to re-partition the origin polygon (in src mesh or cut-mesh) to then recall dispatch
        kernel_output.status == status_t::DETECTED_FLOATING_POLYGON
#endif
    );

    if (convert(kernel_output.status) != McResult::MC_NO_ERROR) {

        context_uptr->log(
            MC_DEBUG_SOURCE_KERNEL,
            MC_DEBUG_TYPE_ERROR,
            0,
            MC_DEBUG_SEVERITY_HIGH,
            to_string(kernel_output.status) + " : " + kernel_output.logger.get_reason_for_failure());

        throw std::runtime_error("incomplete kernel execution");
    }

    TIMESTACK_PUSH("create face partition maps");
    // NOTE: face descriptors in "cut_hmesh_child_to_usermesh_birth_face", need to be offsetted
    // by the number of [internal] source-mesh faces/vertices. This is to ensure consistency with
    // the kernel's data-mapping and make it easier for us to map vertex and face descriptors in
    // connected components to the correct instance in the client input meshes.
    // This offsetting follows a design choice used in the kernel that ("ps-faces" belonging to
    // cut-mesh start [after] the source-mesh faces).
    // Refer to the function "hmesh_to_array_mesh()" on how we use this information.
    std::shared_ptr<std::unordered_map<fd_t, fd_t>> cut_hmesh_child_to_usermesh_birth_face_OFFSETTED = std::shared_ptr<std::unordered_map<fd_t, fd_t>>(new std::unordered_map<fd_t, fd_t>);//cut_hmesh_child_to_usermesh_birth_face;

    for (std::unordered_map<fd_t, fd_t>::iterator i = cut_hmesh_child_to_usermesh_birth_face.begin();
         i != cut_hmesh_child_to_usermesh_birth_face.end(); ++i) {
        fd_t offsettedDescr = fd_t(i->first + source_hmesh.number_of_faces());
        (cut_hmesh_child_to_usermesh_birth_face_OFFSETTED.get()[0])[offsettedDescr] = fd_t(i->second + source_hmesh_face_count_prev); // apply offset
                                                                                                                                     // i->second = fd_t(i->second + source_hmesh_face_count_prev); // apply offset
    }

    std::shared_ptr<std::unordered_map<vd_t, vec3>> cut_hmesh_new_poly_partition_vertices_OFFSETTED = std::shared_ptr<std::unordered_map<vd_t, vec3>>(new std::unordered_map<vd_t, vec3>);

    for (std::unordered_map<vd_t, vec3>::const_iterator i = cut_hmesh_new_poly_partition_vertices.begin();
         i != cut_hmesh_new_poly_partition_vertices.end(); ++i) {
        vd_t offsettedDescr = vd_t(i->first + source_hmesh.number_of_vertices());
        (cut_hmesh_new_poly_partition_vertices_OFFSETTED.get()[0])[offsettedDescr] = i->second; // apply offset
    }

    TIMESTACK_POP();

    //
    // sealed-fragment connected components
    //
    TIMESTACK_PUSH("store sealed-fragment connected components");
    for (std::map<sm_frag_location_t, std::map<cm_patch_location_t, std::vector<output_mesh_info_t>>>::const_iterator i = kernel_output.connected_components.cbegin();
         i != kernel_output.connected_components.cend();
         ++i) {

        for (std::map<cm_patch_location_t, std::vector<output_mesh_info_t>>::const_iterator j = i->second.cbegin();
             j != i->second.cend();
             ++j) {

            // const std::string cs_patch_loc_str = to_string(j->first);

            for (std::vector<output_mesh_info_t>::const_iterator k = j->second.cbegin(); k != j->second.cend(); ++k) {

                std::unique_ptr<connected_component_t, void (*)(connected_component_t*)> frag = std::unique_ptr<fragment_cc_t, void (*)(connected_component_t*)>(new fragment_cc_t, fn_delete_cc<fragment_cc_t>);
                McConnectedComponent clientHandle = reinterpret_cast<McConnectedComponent>(frag.get());
                context_uptr->connected_components.emplace(clientHandle, std::move(frag));
                fragment_cc_t* asFragPtr = dynamic_cast<fragment_cc_t*>(context_uptr->connected_components.at(clientHandle).get());
                asFragPtr->type = MC_CONNECTED_COMPONENT_TYPE_FRAGMENT;
                asFragPtr->fragmentLocation = convert(i->first);
                asFragPtr->patchLocation = convert(j->first);

                MCUT_ASSERT(asFragPtr->patchLocation != MC_PATCH_LOCATION_UNDEFINED);

                asFragPtr->srcMeshSealType = McFragmentSealType::MC_FRAGMENT_SEAL_TYPE_COMPLETE;
                asFragPtr->kernel_hmesh_data = std::move(*k);

                asFragPtr->source_hmesh_child_to_usermesh_birth_face = source_hmesh_child_to_usermesh_birth_face;
                asFragPtr->cut_hmesh_child_to_usermesh_birth_face = cut_hmesh_child_to_usermesh_birth_face_OFFSETTED;
                asFragPtr->source_hmesh_new_poly_partition_vertices=source_hmesh_new_poly_partition_vertices;
                asFragPtr->cut_hmesh_new_poly_partition_vertices = cut_hmesh_new_poly_partition_vertices_OFFSETTED;

                asFragPtr->internal_sourcemesh_vertex_count = source_hmesh.number_of_vertices();
                asFragPtr->client_sourcemesh_vertex_count = numSrcMeshVertices;
                asFragPtr->internal_sourcemesh_face_count = source_hmesh.number_of_faces();
                asFragPtr->client_sourcemesh_face_count = numSrcMeshFaces; // or source_hmesh_face_count

#if 0
                hmesh_to_array_mesh(
#if defined(MCUT_MULTI_THREADED)
                    context_uptr,
#endif
                    asFragPtr->indexArrayMesh,
                    *k,
                    source_hmesh_new_poly_partition_vertices,
                    source_hmesh_child_to_usermesh_birth_face,
                    addedFpPartitioningVerticesOnCutMeshOFFSETTED,
                    fpPartitionChildFaceToInputCutMeshFaceOFFSETTED,
                    numSrcMeshVertices,
                    source_hmesh_face_count,
                    source_hmesh.number_of_vertices(),
                    source_hmesh.number_of_faces());
#endif
            }
        }
    }
    TIMESTACK_POP();

    //
    // unsealed connected components (fragements)
    //
    TIMESTACK_PUSH("store unsealed connected components");
    for (std::map<sm_frag_location_t, std::vector<output_mesh_info_t>>::const_iterator i = kernel_output.unsealed_cc.cbegin();
         i != kernel_output.unsealed_cc.cend();
         ++i) { // for each cc location flag (above/below/undefined)

        for (std::vector<output_mesh_info_t>::const_iterator j = i->second.cbegin(); j != i->second.cend(); ++j) { // for each mesh

            std::unique_ptr<connected_component_t, void (*)(connected_component_t*)> unsealedFrag = std::unique_ptr<fragment_cc_t, void (*)(connected_component_t*)>(new fragment_cc_t, fn_delete_cc<fragment_cc_t>);
            McConnectedComponent clientHandle = reinterpret_cast<McConnectedComponent>(unsealedFrag.get());
            context_uptr->connected_components.emplace(clientHandle, std::move(unsealedFrag));
            fragment_cc_t* asFragPtr = dynamic_cast<fragment_cc_t*>(context_uptr->connected_components.at(clientHandle).get());
            asFragPtr->type = MC_CONNECTED_COMPONENT_TYPE_FRAGMENT;
            asFragPtr->fragmentLocation = convert(i->first);
            asFragPtr->patchLocation = McPatchLocation::MC_PATCH_LOCATION_UNDEFINED;
            asFragPtr->srcMeshSealType = McFragmentSealType::MC_FRAGMENT_SEAL_TYPE_NONE;

            asFragPtr->kernel_hmesh_data = std::move(*j);

            asFragPtr->source_hmesh_child_to_usermesh_birth_face = source_hmesh_child_to_usermesh_birth_face;
                asFragPtr->cut_hmesh_child_to_usermesh_birth_face = cut_hmesh_child_to_usermesh_birth_face_OFFSETTED;
                asFragPtr->source_hmesh_new_poly_partition_vertices=source_hmesh_new_poly_partition_vertices;
                asFragPtr->cut_hmesh_new_poly_partition_vertices = cut_hmesh_new_poly_partition_vertices_OFFSETTED;

                asFragPtr->internal_sourcemesh_vertex_count = source_hmesh.number_of_vertices();
                asFragPtr->client_sourcemesh_vertex_count = numSrcMeshVertices;
                asFragPtr->internal_sourcemesh_face_count = source_hmesh.number_of_faces();
                asFragPtr->client_sourcemesh_face_count = numSrcMeshFaces; // or source_hmesh_face_count

           
#if 0
            hmesh_to_array_mesh(
#if defined(MCUT_MULTI_THREADED)
                context_uptr,
#endif
                asFragPtr->indexArrayMesh,
                *j,
                source_hmesh_new_poly_partition_vertices,
                source_hmesh_child_to_usermesh_birth_face,
                addedFpPartitioningVerticesOnCutMeshOFFSETTED,
                fpPartitionChildFaceToInputCutMeshFaceOFFSETTED,
                numSrcMeshVertices,
                source_hmesh_face_count,
                source_hmesh.number_of_vertices(),
                source_hmesh.number_of_faces());

#endif
        }
    }
    TIMESTACK_POP();

    // inside patches
    TIMESTACK_PUSH("store interior patches");
    const std::vector<output_mesh_info_t>& insidePatches = kernel_output.inside_patches[cm_patch_winding_order_t::DEFAULT];

    for (std::vector<output_mesh_info_t>::const_iterator it = insidePatches.cbegin();
         it != insidePatches.cend();
         ++it) {

        std::unique_ptr<connected_component_t, void (*)(connected_component_t*)> patchConnComp = std::unique_ptr<patch_cc_t, void (*)(connected_component_t*)>(new patch_cc_t, fn_delete_cc<patch_cc_t>);
        McConnectedComponent clientHandle = reinterpret_cast<McConnectedComponent>(patchConnComp.get());
        context_uptr->connected_components.emplace(clientHandle, std::move(patchConnComp));
        patch_cc_t* asPatchPtr = dynamic_cast<patch_cc_t*>(context_uptr->connected_components.at(clientHandle).get());
        asPatchPtr->type = MC_CONNECTED_COMPONENT_TYPE_PATCH;
        asPatchPtr->patchLocation = MC_PATCH_LOCATION_INSIDE;

        asPatchPtr->kernel_hmesh_data = std::move(*it);

        asPatchPtr->source_hmesh_child_to_usermesh_birth_face = source_hmesh_child_to_usermesh_birth_face;
                asPatchPtr->cut_hmesh_child_to_usermesh_birth_face = cut_hmesh_child_to_usermesh_birth_face_OFFSETTED;
                asPatchPtr->source_hmesh_new_poly_partition_vertices=source_hmesh_new_poly_partition_vertices;
                asPatchPtr->cut_hmesh_new_poly_partition_vertices = cut_hmesh_new_poly_partition_vertices_OFFSETTED;

                asPatchPtr->internal_sourcemesh_vertex_count = source_hmesh.number_of_vertices();
                asPatchPtr->client_sourcemesh_vertex_count = numSrcMeshVertices;
                asPatchPtr->internal_sourcemesh_face_count = source_hmesh.number_of_faces();
                asPatchPtr->client_sourcemesh_face_count = numSrcMeshFaces; // or source_hmesh_face_count

#if 0
        hmesh_to_array_mesh(
#if defined(MCUT_MULTI_THREADED)
            context_uptr,
#endif
            asPatchPtr->indexArrayMesh,
            *it,
            source_hmesh_new_poly_partition_vertices,
            source_hmesh_child_to_usermesh_birth_face,
            addedFpPartitioningVerticesOnCutMeshOFFSETTED,
            fpPartitionChildFaceToInputCutMeshFaceOFFSETTED,
            numSrcMeshVertices,
            source_hmesh_face_count,
            source_hmesh.number_of_vertices(),
            source_hmesh.number_of_faces());
#endif
    }
    TIMESTACK_POP();

    // outside patches
    TIMESTACK_PUSH("store exterior patches");
    const std::vector<output_mesh_info_t>& outsidePatches = kernel_output.outside_patches[cm_patch_winding_order_t::DEFAULT];

    for (std::vector<output_mesh_info_t>::const_iterator it = outsidePatches.cbegin(); it != outsidePatches.cend(); ++it) {

        std::unique_ptr<connected_component_t, void (*)(connected_component_t*)> patchConnComp = std::unique_ptr<patch_cc_t, void (*)(connected_component_t*)>(new patch_cc_t, fn_delete_cc<patch_cc_t>);
        McConnectedComponent clientHandle = reinterpret_cast<McConnectedComponent>(patchConnComp.get());
        context_uptr->connected_components.emplace(clientHandle, std::move(patchConnComp));
        patch_cc_t* asPatchPtr = dynamic_cast<patch_cc_t*>(context_uptr->connected_components.at(clientHandle).get());
        asPatchPtr->type = MC_CONNECTED_COMPONENT_TYPE_PATCH;
        asPatchPtr->patchLocation = MC_PATCH_LOCATION_OUTSIDE;
        asPatchPtr->kernel_hmesh_data = std::move(*it);

        asPatchPtr->source_hmesh_child_to_usermesh_birth_face = source_hmesh_child_to_usermesh_birth_face;
        asPatchPtr->cut_hmesh_child_to_usermesh_birth_face = cut_hmesh_child_to_usermesh_birth_face_OFFSETTED;
        asPatchPtr->source_hmesh_new_poly_partition_vertices=source_hmesh_new_poly_partition_vertices;
        asPatchPtr->cut_hmesh_new_poly_partition_vertices = cut_hmesh_new_poly_partition_vertices_OFFSETTED;

        asPatchPtr->internal_sourcemesh_vertex_count = source_hmesh.number_of_vertices();
        asPatchPtr->client_sourcemesh_vertex_count = numSrcMeshVertices;
        asPatchPtr->internal_sourcemesh_face_count = source_hmesh.number_of_faces();
        asPatchPtr->client_sourcemesh_face_count = numSrcMeshFaces; // or source_hmesh_face_count

#if 0
            hmesh_to_array_mesh(
#if defined(MCUT_MULTI_THREADED)
                context_uptr,
#endif
                asPatchPtr->indexArrayMesh,
                *it,
                source_hmesh_new_poly_partition_vertices,
                source_hmesh_child_to_usermesh_birth_face, addedFpPartitioningVerticesOnCutMeshOFFSETTED, fpPartitionChildFaceToInputCutMeshFaceOFFSETTED,
                numSrcMeshVertices, source_hmesh_face_count, source_hmesh.number_of_vertices(), source_hmesh.number_of_faces());
#endif
    }
    TIMESTACK_POP();

    // seam connected components
    // -------------------------

    // NOTE: seam meshes are not available if there was a partial cut intersection (due to constraints imposed by halfedge construction rules).

    //  src mesh

    if (kernel_output.seamed_src_mesh.mesh.number_of_faces() > 0) {
        TIMESTACK_PUSH("store source-mesh seam");
        std::unique_ptr<connected_component_t, void (*)(connected_component_t*)> srcMeshSeam = std::unique_ptr<seam_cc_t, void (*)(connected_component_t*)>(new seam_cc_t, fn_delete_cc<seam_cc_t>);
        McConnectedComponent clientHandle = reinterpret_cast<McConnectedComponent>(srcMeshSeam.get());
        context_uptr->connected_components.emplace(clientHandle, std::move(srcMeshSeam));
        seam_cc_t* asSrcMeshSeamPtr = dynamic_cast<seam_cc_t*>(context_uptr->connected_components.at(clientHandle).get());
        asSrcMeshSeamPtr->type = MC_CONNECTED_COMPONENT_TYPE_SEAM;
        asSrcMeshSeamPtr->origin = MC_SEAM_ORIGIN_SRCMESH;

        asSrcMeshSeamPtr->kernel_hmesh_data = std::move(kernel_output.seamed_src_mesh);

        asSrcMeshSeamPtr->source_hmesh_child_to_usermesh_birth_face = source_hmesh_child_to_usermesh_birth_face;
        asSrcMeshSeamPtr->cut_hmesh_child_to_usermesh_birth_face = cut_hmesh_child_to_usermesh_birth_face_OFFSETTED;
        asSrcMeshSeamPtr->source_hmesh_new_poly_partition_vertices=source_hmesh_new_poly_partition_vertices;
        asSrcMeshSeamPtr->cut_hmesh_new_poly_partition_vertices = cut_hmesh_new_poly_partition_vertices_OFFSETTED;

        asSrcMeshSeamPtr->internal_sourcemesh_vertex_count = source_hmesh.number_of_vertices();
        asSrcMeshSeamPtr->client_sourcemesh_vertex_count = numSrcMeshVertices;
        asSrcMeshSeamPtr->internal_sourcemesh_face_count = source_hmesh.number_of_faces();
        asSrcMeshSeamPtr->client_sourcemesh_face_count = numSrcMeshFaces; // or source_hmesh_face_count
#if 0
            hmesh_to_array_mesh(
#if defined(MCUT_MULTI_THREADED)
                context_uptr,
#endif
                asSrcMeshSeamPtr->indexArrayMesh, kernel_output.seamed_src_mesh,
                source_hmesh_new_poly_partition_vertices, source_hmesh_child_to_usermesh_birth_face, addedFpPartitioningVerticesOnCutMeshOFFSETTED, fpPartitionChildFaceToInputCutMeshFaceOFFSETTED,
                numSrcMeshVertices, source_hmesh_face_count, source_hmesh.number_of_vertices(), source_hmesh.number_of_faces());
#endif
        TIMESTACK_POP();
    }

    //  cut mesh

    if (kernel_output.seamed_cut_mesh.mesh.number_of_faces() > 0) {
        TIMESTACK_PUSH("store cut-mesh seam");

        std::unique_ptr<connected_component_t, void (*)(connected_component_t*)> cutMeshSeam = std::unique_ptr<seam_cc_t, void (*)(connected_component_t*)>(new seam_cc_t, fn_delete_cc<seam_cc_t>);
        McConnectedComponent clientHandle = reinterpret_cast<McConnectedComponent>(cutMeshSeam.get());
        context_uptr->connected_components.emplace(clientHandle, std::move(cutMeshSeam));
        seam_cc_t* asCutMeshSeamPtr = dynamic_cast<seam_cc_t*>(context_uptr->connected_components.at(clientHandle).get());
        asCutMeshSeamPtr->type = MC_CONNECTED_COMPONENT_TYPE_SEAM;
        asCutMeshSeamPtr->origin = MC_SEAM_ORIGIN_CUTMESH;

        asCutMeshSeamPtr->kernel_hmesh_data = std::move(kernel_output.seamed_cut_mesh);

        asCutMeshSeamPtr->source_hmesh_child_to_usermesh_birth_face = source_hmesh_child_to_usermesh_birth_face;
        asCutMeshSeamPtr->cut_hmesh_child_to_usermesh_birth_face = cut_hmesh_child_to_usermesh_birth_face_OFFSETTED;
        asCutMeshSeamPtr->source_hmesh_new_poly_partition_vertices=source_hmesh_new_poly_partition_vertices;
        asCutMeshSeamPtr->cut_hmesh_new_poly_partition_vertices = cut_hmesh_new_poly_partition_vertices_OFFSETTED;

        asCutMeshSeamPtr->internal_sourcemesh_vertex_count = source_hmesh.number_of_vertices();
        asCutMeshSeamPtr->client_sourcemesh_vertex_count = numSrcMeshVertices;
        asCutMeshSeamPtr->internal_sourcemesh_face_count = source_hmesh.number_of_faces();
        asCutMeshSeamPtr->client_sourcemesh_face_count = numSrcMeshFaces; // or source_hmesh_face_count
#if 0
            hmesh_to_array_mesh(
#if defined(MCUT_MULTI_THREADED)
                context_uptr,
#endif
                asCutMeshSeamPtr->indexArrayMesh, kernel_output.seamed_cut_mesh,
                source_hmesh_new_poly_partition_vertices, source_hmesh_child_to_usermesh_birth_face, addedFpPartitioningVerticesOnCutMeshOFFSETTED, fpPartitionChildFaceToInputCutMeshFaceOFFSETTED,
                numSrcMeshVertices, source_hmesh_face_count, source_hmesh.number_of_vertices(), source_hmesh.number_of_faces());
#endif
        TIMESTACK_POP();
    }

    // input connected components
    // --------------------------

    // internal cut-mesh (possibly with new faces and vertices)
    {
        TIMESTACK_PUSH("store original cut-mesh");
        std::unique_ptr<connected_component_t, void (*)(connected_component_t*)> internalCutMesh = std::unique_ptr<input_cc_t, void (*)(connected_component_t*)>(new input_cc_t, fn_delete_cc<input_cc_t>);
        McConnectedComponent clientHandle = reinterpret_cast<McConnectedComponent>(internalCutMesh.get());
        context_uptr->connected_components.emplace(clientHandle, std::move(internalCutMesh));
        input_cc_t* asCutMeshInputPtr = dynamic_cast<input_cc_t*>(context_uptr->connected_components.at(clientHandle).get());
        asCutMeshInputPtr->type = MC_CONNECTED_COMPONENT_TYPE_INPUT;
        asCutMeshInputPtr->origin = MC_INPUT_ORIGIN_CUTMESH;

        output_mesh_info_t omi;
        omi.mesh = cut_hmesh; // naive copy (could use std::move)

        // TODO: assume that re-adding elements (vertices and faces) e.g. prior to perturbation or partitioning is going to change the order
        // from the user-provided order. So we still need to fix the mapping, which may no longer
        // be one-to-one (even if with an sm offset ) as in the case when things do not change.

        if (kernel_input.populate_vertex_maps) {
            omi.data_maps.vertex_map.resize(cut_hmesh.number_of_vertices());
            // TODO: make parallel
            for (vertex_array_iterator_t i = cut_hmesh.vertices_begin(); i != cut_hmesh.vertices_end(); ++i) {
                omi.data_maps.vertex_map[*i] = vd_t((*i) + source_hmesh.number_of_vertices()); // apply offset like kernel does
            }
        }

        if (kernel_input.populate_face_maps) {
            omi.data_maps.face_map.resize(cut_hmesh.number_of_faces());
            // TODO: make parallel
            for (face_array_iterator_t i = cut_hmesh.faces_begin(); i != cut_hmesh.faces_end(); ++i) {
                omi.data_maps.face_map[*i] = fd_t((*i) + source_hmesh.number_of_faces()); // apply offset like kernel does
            }
        }

        omi.seam_vertices = {}; // empty. an input connected component has no polygon intersection points

        asCutMeshInputPtr->kernel_hmesh_data = std::move(omi);

        asCutMeshInputPtr->source_hmesh_child_to_usermesh_birth_face = source_hmesh_child_to_usermesh_birth_face;
        asCutMeshInputPtr->cut_hmesh_child_to_usermesh_birth_face = cut_hmesh_child_to_usermesh_birth_face_OFFSETTED;
        asCutMeshInputPtr->source_hmesh_new_poly_partition_vertices=source_hmesh_new_poly_partition_vertices;
        asCutMeshInputPtr->cut_hmesh_new_poly_partition_vertices = cut_hmesh_new_poly_partition_vertices_OFFSETTED;

        asCutMeshInputPtr->internal_sourcemesh_vertex_count = source_hmesh.number_of_vertices();
        asCutMeshInputPtr->client_sourcemesh_vertex_count = numSrcMeshVertices;
        asCutMeshInputPtr->internal_sourcemesh_face_count = source_hmesh.number_of_faces();
        asCutMeshInputPtr->client_sourcemesh_face_count = numSrcMeshFaces; // or source_hmesh_face_count

#if 0
            hmesh_to_array_mesh(
#if defined(MCUT_MULTI_THREADED)
                context_uptr,
#endif
                asCutMeshInputPtr->indexArrayMesh, omi,
                source_hmesh_new_poly_partition_vertices, source_hmesh_child_to_usermesh_birth_face, addedFpPartitioningVerticesOnCutMeshOFFSETTED, fpPartitionChildFaceToInputCutMeshFaceOFFSETTED,
                numSrcMeshVertices, source_hmesh_face_count, source_hmesh.number_of_vertices(), source_hmesh.number_of_faces());
#endif

        TIMESTACK_POP();
    }

    // internal source-mesh (possibly with new faces and vertices)
    {
        TIMESTACK_PUSH("store original src-mesh");
        std::unique_ptr<connected_component_t, void (*)(connected_component_t*)> internalSrcMesh = std::unique_ptr<input_cc_t, void (*)(connected_component_t*)>(new input_cc_t, fn_delete_cc<input_cc_t>);
        McConnectedComponent clientHandle = reinterpret_cast<McConnectedComponent>(internalSrcMesh.get());
        context_uptr->connected_components.emplace(clientHandle, std::move(internalSrcMesh));
        input_cc_t* asSrcMeshInputPtr = dynamic_cast<input_cc_t*>(context_uptr->connected_components.at(clientHandle).get());
        asSrcMeshInputPtr->type = MC_CONNECTED_COMPONENT_TYPE_INPUT;
        asSrcMeshInputPtr->origin = MC_INPUT_ORIGIN_SRCMESH;

        output_mesh_info_t omi;
        omi.mesh = source_hmesh; // naive copy

        if (kernel_input.populate_vertex_maps) {
            omi.data_maps.vertex_map.resize(source_hmesh.number_of_vertices());
            // TODO: make parallel
            for (vertex_array_iterator_t i = source_hmesh.vertices_begin(); i != source_hmesh.vertices_end(); ++i) {
                omi.data_maps.vertex_map[*i] = *i; // one to one mapping
            }
        }

        if (kernel_input.populate_face_maps) {
            omi.data_maps.face_map.resize(source_hmesh.number_of_faces());
            // TODO: make parallel
            for (face_array_iterator_t i = source_hmesh.faces_begin(); i != source_hmesh.faces_end(); ++i) {
                omi.data_maps.face_map[*i] = *i; // one to one mapping
            }
        }

        omi.seam_vertices = {}; // empty. an input connected component has no polygon intersection points

        asSrcMeshInputPtr->kernel_hmesh_data = std::move(omi);

        asSrcMeshInputPtr->source_hmesh_child_to_usermesh_birth_face = source_hmesh_child_to_usermesh_birth_face;
        asSrcMeshInputPtr->cut_hmesh_child_to_usermesh_birth_face = cut_hmesh_child_to_usermesh_birth_face_OFFSETTED;
        asSrcMeshInputPtr->source_hmesh_new_poly_partition_vertices=source_hmesh_new_poly_partition_vertices;
        asSrcMeshInputPtr->cut_hmesh_new_poly_partition_vertices = cut_hmesh_new_poly_partition_vertices_OFFSETTED;

        asSrcMeshInputPtr->internal_sourcemesh_vertex_count = source_hmesh.number_of_vertices();
        asSrcMeshInputPtr->client_sourcemesh_vertex_count = numSrcMeshVertices;
        asSrcMeshInputPtr->internal_sourcemesh_face_count = source_hmesh.number_of_faces();
        asSrcMeshInputPtr->client_sourcemesh_face_count = numSrcMeshFaces; // or source_hmesh_face_count

#if 0
            hmesh_to_array_mesh(
#if defined(MCUT_MULTI_THREADED)
                context_uptr,
#endif
                asSrcMeshInputPtr->indexArrayMesh, omi,
                source_hmesh_new_poly_partition_vertices, source_hmesh_child_to_usermesh_birth_face, addedFpPartitioningVerticesOnCutMeshOFFSETTED, fpPartitionChildFaceToInputCutMeshFaceOFFSETTED,
                numSrcMeshVertices, source_hmesh_face_count, source_hmesh.number_of_vertices(), source_hmesh.number_of_faces());
#endif

        TIMESTACK_POP();
    }
}
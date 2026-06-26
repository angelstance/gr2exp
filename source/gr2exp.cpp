#include <max.h>
#include <maxapi.h>
#include <triobj.h>
#include <meshnormalspec.h>
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "gr2exp.h"
#include "gr2expres.h"

#include "granny.h"

HINSTANCE hInstance;

namespace {

const Class_ID kGr2ExporterClassId(0x0dbf3c3a, 0x53d1450e);
constexpr granny_uint32 kGr2ExporterTag = 0x47523201u;

const TCHAR* GetString(int id) {
    static TCHAR buf[256];
    if (hInstance) {
        return LoadString(hInstance, id, buf, _countof(buf)) ? buf : nullptr;
    }
    return nullptr;
}

std::wstring ToWide(const TCHAR* value) {
    return value ? std::wstring(value) : std::wstring();
}

std::string ToNarrow(const std::wstring& value) {
    if (value.empty()) {
        return std::string();
    }

    const int length = WideCharToMultiByte(CP_ACP, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return std::string();
    }

    std::string converted(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_ACP, 0, value.c_str(), -1, converted.data(), length, nullptr, nullptr);
    converted.resize(static_cast<size_t>(length - 1));
    return converted;
}

void ShowError(const std::wstring& message) {
    MessageBoxW(nullptr, message.c_str(), L"GR2 Exporter", MB_OK | MB_ICONERROR);
}

std::wstring NodeNameOrFallback(INode* node, int index) {
    if (node) {
        const MCHAR* name = node->GetName();
        if (name && *name) {
            return ToWide(name);
        }
    }
    return L"gr2_mesh_" + std::to_wstring(index);
}

bool ShouldExportNode(INode* node, bool exportSelected) {
    if (!node || node->IsRootNode()) {
        return false;
    }
    if (exportSelected && !node->Selected()) {
        return false;
    }
    Object* object = node->EvalWorldState(GetCOREInterface()->GetTime()).obj;
    return object && object->CanConvertToType(triObjectClassID);
}

void CollectExportNodes(INode* node, bool exportSelected, std::vector<INode*>& nodes) {
    if (!node) {
        return;
    }

    if (ShouldExportNode(node, exportSelected)) {
        nodes.push_back(node);
    }

    for (int childIndex = 0; childIndex < node->NumberOfChildren(); ++childIndex) {
        CollectExportNodes(node->GetChildNode(childIndex), exportSelected, nodes);
    }
}

class ExportPackage {
public:
    ExportPackage() {
        std::memset(&fileInfo_, 0, sizeof(fileInfo_));

        artToolInfo_ = std::make_unique<granny_art_tool_info>();
        std::memset(artToolInfo_.get(), 0, sizeof(granny_art_tool_info));
        artToolInfo_->FromArtToolName = internString("3ds Max");
        artToolInfo_->ArtToolMajorRevision = 2026;
        artToolInfo_->ArtToolMinorRevision = 0;
        artToolInfo_->ArtToolPointerSize = static_cast<granny_int32>(sizeof(void*));
        artToolInfo_->UnitsPerMeter = 1.0f;
        artToolInfo_->RightVector[0] = 1.0f;
        artToolInfo_->UpVector[2] = 1.0f;
        artToolInfo_->BackVector[1] = -1.0f;

        exporterInfo_ = std::make_unique<granny_exporter_info>();
        std::memset(exporterInfo_.get(), 0, sizeof(granny_exporter_info));
        exporterInfo_->ExporterName = const_cast<char*>(internString("Custom GR2 Exporter"));
        exporterInfo_->ExporterMajorRevision = 1;
        exporterInfo_->ExporterMinorRevision = 0;
        exporterInfo_->ExporterCustomization = 0;
        exporterInfo_->ExporterBuildNumber = 1;

        fileInfo_.ArtToolInfo = artToolInfo_.get();
        fileInfo_.ExporterInfo = exporterInfo_.get();
    }

    bool addNode(INode* node, TimeValue time, int meshIndex, std::wstring& error) {
        ObjectState state = node->EvalWorldState(time);
        Object* object = state.obj;
        if (!object || !object->CanConvertToType(triObjectClassID)) {
            return false;
        }

        TriObject* triObject = static_cast<TriObject*>(object->ConvertToType(time, triObjectClassID));
        if (!triObject) {
            error = L"Unable to convert object to TriObject.";
            return false;
        }

        const bool mustDeleteTriObject = (triObject != object);
        Mesh& mesh = triObject->GetMesh();
        mesh.buildNormals();

        const int faceCount = mesh.getNumFaces();
        if (faceCount <= 0) {
            if (mustDeleteTriObject) {
                triObject->DeleteMe();
            }
            return false;
        }

        const Matrix3 transform = node->GetObjTMAfterWSM(time);
        const bool hasTVerts = mesh.getNumTVerts() > 0 && mesh.tvFace != nullptr;
        MeshNormalSpec* normalSpec = mesh.GetSpecifiedNormals();
        if (normalSpec && !(normalSpec->GetFlag(MESH_NORMAL_NORMALS_BUILT))) {
            normalSpec = nullptr;
        }

        // Sort faces by material ID so we can build contiguous material groups.
        std::vector<int> faceOrder(static_cast<size_t>(faceCount));
        for (int i = 0; i < faceCount; ++i) faceOrder[i] = i;
        std::stable_sort(faceOrder.begin(), faceOrder.end(), [&](int a, int b) {
            return mesh.faces[a].getMatID() < mesh.faces[b].getMatID();
        });

        // Build material groups.
        std::vector<granny_tri_material_group> groupVec;
        {
            int groupStart = 0;
            while (groupStart < faceCount) {
                const MtlID matID = mesh.faces[faceOrder[groupStart]].getMatID();
                int groupEnd = groupStart + 1;
                while (groupEnd < faceCount && mesh.faces[faceOrder[groupEnd]].getMatID() == matID) {
                    ++groupEnd;
                }
                granny_tri_material_group g;
                g.MaterialIndex = static_cast<granny_int32>(matID);
                g.TriFirst = groupStart;
                g.TriCount = groupEnd - groupStart;
                groupVec.push_back(g);
                groupStart = groupEnd;
            }
        }

        const int exportedVertexCount = faceCount * 3;
        auto vertexBuffer = std::make_unique<granny_pnt332_vertex[]>(static_cast<size_t>(exportedVertexCount));
        auto indexBuffer = std::make_unique<granny_int32[]>(static_cast<size_t>(exportedVertexCount));
        auto groups = std::make_unique<granny_tri_material_group[]>(groupVec.size());
        std::memcpy(groups.get(), groupVec.data(), groupVec.size() * sizeof(granny_tri_material_group));

        for (int sortedIndex = 0; sortedIndex < faceCount; ++sortedIndex) {
            const int faceIndex = faceOrder[sortedIndex];
            Face& face = mesh.faces[faceIndex];

            for (int corner = 0; corner < 3; ++corner) {
                const int exportVertexIndex = (sortedIndex * 3) + corner;
                const int sourceVertexIndex = face.v[corner];
                const Point3 position = transform.PointTransform(mesh.verts[sourceVertexIndex]);

                // Per-vertex smoothed normal: prefer MeshNormalSpec, fall back to RVertex.
                auto getSmoothedNormal = [&]() -> Point3 {
                    if (normalSpec) {
                        return Normalize(VectorTransform(transform, normalSpec->GetNormal(faceIndex, corner)));
                    }
                    const RVertex* rvPtr = mesh.getRVertPtr(sourceVertexIndex);
                    if (!rvPtr) {
                        return Point3(0.0f, 0.0f, 1.0f);
                    }
                    const RVertex& rv = *rvPtr;
                    const int numNormals = static_cast<int>(rv.rFlags & NORCT_MASK);
                    if (numNormals == 1 || face.smGroup == 0) {
                        return Normalize(VectorTransform(transform, rv.rn.getNormal()));
                    }
                    for (int k = 0; k < numNormals; ++k) {
                        if (rv.ern[k].getSmGroup() & face.smGroup) {
                            return Normalize(VectorTransform(transform, rv.ern[k].getNormal()));
                        }
                    }
                    return Normalize(VectorTransform(transform, rv.rn.getNormal()));
                };
                const Point3 normal = getSmoothedNormal();

                auto& outVertex = vertexBuffer[static_cast<size_t>(exportVertexIndex)];
                outVertex.Position[0] = position.x;
                outVertex.Position[1] = position.y;
                outVertex.Position[2] = position.z;
                outVertex.Normal[0] = normal.x;
                outVertex.Normal[1] = normal.y;
                outVertex.Normal[2] = normal.z;
                outVertex.UV[0] = 0.0f;
                outVertex.UV[1] = 0.0f;

                if (hasTVerts) {
                    const int tvIndex = mesh.tvFace[faceIndex].t[corner];
                    if (tvIndex >= 0 && tvIndex < mesh.getNumTVerts()) {
                        const UVVert& uv = mesh.tVerts[tvIndex];
                        outVertex.UV[0] = uv.x;
                        outVertex.UV[1] = 1.0f - uv.y;
                    }
                }

                indexBuffer[static_cast<size_t>(exportVertexIndex)] = exportVertexIndex;
            }
        }

        auto vertexData = std::make_unique<granny_vertex_data>();
        std::memset(vertexData.get(), 0, sizeof(granny_vertex_data));
        vertexData->VertexType = GrannyPNT332VertexType;
        vertexData->VertexCount = exportedVertexCount;
        vertexData->Vertices = reinterpret_cast<granny_uint8*>(vertexBuffer.get());

        auto topology = std::make_unique<granny_tri_topology>();
        std::memset(topology.get(), 0, sizeof(granny_tri_topology));
        topology->GroupCount = static_cast<granny_int32>(groupVec.size());
        topology->Groups = groups.get();
        topology->IndexCount = exportedVertexCount;
        topology->Indices = indexBuffer.get();

        auto material = std::make_unique<granny_material>();
        std::memset(material.get(), 0, sizeof(granny_material));
        const std::wstring nodeName = NodeNameOrFallback(node, meshIndex);
        const std::string nodeNameNarrow = ToNarrow(nodeName);
        material->Name = internString(nodeNameNarrow + "_material");

        auto materialBindings = std::make_unique<granny_material_binding[]>(1);
        std::memset(materialBindings.get(), 0, sizeof(granny_material_binding));
        materialBindings[0].Material = material.get();

        auto grannyMesh = std::make_unique<granny_mesh>();
        std::memset(grannyMesh.get(), 0, sizeof(granny_mesh));
        grannyMesh->Name = internString(nodeNameNarrow);
        grannyMesh->PrimaryVertexData = vertexData.get();
        grannyMesh->PrimaryTopology = topology.get();
        grannyMesh->MaterialBindingCount = 1;
        grannyMesh->MaterialBindings = materialBindings.get();

        auto modelBindings = std::make_unique<granny_model_mesh_binding[]>(1);
        std::memset(modelBindings.get(), 0, sizeof(granny_model_mesh_binding));
        modelBindings[0].Mesh = grannyMesh.get();

        auto model = std::make_unique<granny_model>();
        std::memset(model.get(), 0, sizeof(granny_model));
        model->Name = internString(nodeNameNarrow);
        GrannyMakeIdentity(&model->InitialPlacement);
        model->MeshBindingCount = 1;
        model->MeshBindings = modelBindings.get();

        materials_.push_back(std::move(material));
        vertexBuffers_.push_back(std::move(vertexBuffer));
        indexBuffers_.push_back(std::move(indexBuffer));
        groupBuffers_.push_back(std::move(groups));
        vertexDatas_.push_back(std::move(vertexData));
        topologies_.push_back(std::move(topology));
        materialBindings_.push_back(std::move(materialBindings));
        meshes_.push_back(std::move(grannyMesh));
        modelBindings_.push_back(std::move(modelBindings));
        models_.push_back(std::move(model));

        if (mustDeleteTriObject) {
            triObject->DeleteMe();
        }

        return true;
    }

    bool writeToFile(const std::wstring& outputPath, std::wstring& error) {
        fileInfo_.FromFileName = internString(ToNarrow(outputPath));

        materialPointerArray_ = buildPointerArray(materials_);
        vertexDataPointerArray_ = buildPointerArray(vertexDatas_);
        topologyPointerArray_ = buildPointerArray(topologies_);
        meshPointerArray_ = buildPointerArray(meshes_);
        modelPointerArray_ = buildPointerArray(models_);

        fileInfo_.MaterialCount = static_cast<granny_int32>(materials_.size());
        fileInfo_.Materials = materialPointerArray_.get();
        fileInfo_.VertexDataCount = static_cast<granny_int32>(vertexDatas_.size());
        fileInfo_.VertexDatas = vertexDataPointerArray_.get();
        fileInfo_.TriTopologyCount = static_cast<granny_int32>(topologies_.size());
        fileInfo_.TriTopologies = topologyPointerArray_.get();
        fileInfo_.MeshCount = static_cast<granny_int32>(meshes_.size());
        fileInfo_.Meshes = meshPointerArray_.get();
        fileInfo_.ModelCount = static_cast<granny_int32>(models_.size());
        fileInfo_.Models = modelPointerArray_.get();

        const std::string outputPathNarrow = ToNarrow(outputPath);
        granny_file_builder* builder = GrannyBeginFile(
            1,
            kGr2ExporterTag,
            GrannyGRNFileMV_ThisPlatform,
            GrannyGetTemporaryDirectory(),
            "gr2exp");
        if (!builder) {
            error = L"GrannyBeginFile failed.";
            return false;
        }

        granny_file_data_tree_writer* writer =
            GrannyBeginFileDataTreeWriting(GrannyFileInfoType, &fileInfo_, 0, 0);
        if (!writer) {
            GrannyAbortFile(builder);
            error = L"GrannyBeginFileDataTreeWriting failed.";
            return false;
        }

        const bool writeOk = GrannyWriteDataTreeToFileBuilder(writer, builder);
        GrannyEndFileDataTreeWriting(writer);
        if (!writeOk) {
            GrannyAbortFile(builder);
            error = L"GrannyWriteDataTreeToFileBuilder failed.";
            return false;
        }

        if (!GrannyEndFile(builder, outputPathNarrow.c_str())) {
            error = L"GrannyEndFile failed while writing the GR2 file.";
            return false;
        }

        return true;
    }

private:
    template <typename T>
    static std::unique_ptr<T*[]> buildPointerArray(const std::vector<std::unique_ptr<T>>& objects) {
        if (objects.empty()) {
            return nullptr;
        }

        auto result = std::make_unique<T*[]>(objects.size());
        for (size_t index = 0; index < objects.size(); ++index) {
            result[index] = objects[index].get();
        }
        return result;
    }

    char* internString(const std::string& value) {
        auto buffer = std::make_unique<char[]>(value.size() + 1);
        std::memcpy(buffer.get(), value.c_str(), value.size() + 1);
        char* raw = buffer.get();
        ownedStrings_.push_back(std::move(buffer));
        return raw;
    }

    granny_file_info fileInfo_{};
    std::unique_ptr<granny_art_tool_info> artToolInfo_;
    std::unique_ptr<granny_exporter_info> exporterInfo_;
    std::deque<std::unique_ptr<char[]>> ownedStrings_;

    std::vector<std::unique_ptr<granny_material>> materials_;
    std::vector<std::unique_ptr<granny_vertex_data>> vertexDatas_;
    std::vector<std::unique_ptr<granny_tri_topology>> topologies_;
    std::vector<std::unique_ptr<granny_mesh>> meshes_;
    std::vector<std::unique_ptr<granny_model>> models_;

    std::vector<std::unique_ptr<granny_pnt332_vertex[]>> vertexBuffers_;
    std::vector<std::unique_ptr<granny_int32[]>> indexBuffers_;
    std::vector<std::unique_ptr<granny_tri_material_group[]>> groupBuffers_;
    std::vector<std::unique_ptr<granny_material_binding[]>> materialBindings_;
    std::vector<std::unique_ptr<granny_model_mesh_binding[]>> modelBindings_;

    std::unique_ptr<granny_material*[]> materialPointerArray_;
    std::unique_ptr<granny_vertex_data*[]> vertexDataPointerArray_;
    std::unique_ptr<granny_tri_topology*[]> topologyPointerArray_;
    std::unique_ptr<granny_mesh*[]> meshPointerArray_;
    std::unique_ptr<granny_model*[]> modelPointerArray_;
};

class GR2ExportClassDesc : public ClassDesc {
public:
    int IsPublic() override { return 1; }
    void* Create(BOOL = FALSE) override { return new GR2Export(); }
    const TCHAR* ClassName() override { return GetString(IDS_GR2_EXPORTER); }
    const TCHAR* NonLocalizedClassName() override { return _T("GR2Exporter"); }
    SClass_ID SuperClassID() override { return SCENE_EXPORT_CLASS_ID; }
    Class_ID ClassID() override { return kGr2ExporterClassId; }
    const TCHAR* Category() override { return GetString(IDS_SCENEEXPORT); }
};

GR2ExportClassDesc gGr2ExportClassDesc;

} // namespace

BOOL WINAPI DllMain(HINSTANCE hinstDLL, ULONG fdwReason, LPVOID) {
    hInstance = hinstDLL;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        MaxSDK::Util::UseLanguagePackLocale();
        DisableThreadLibraryCalls(hInstance);
    }
    return TRUE;
}

__declspec(dllexport) const TCHAR* LibDescription() {
    return GetString(IDS_LIBDESCRIPTION);
}

__declspec(dllexport) int LibNumberClasses() {
    return 1;
}

__declspec(dllexport) ClassDesc* LibClassDesc(int i) {
    return (i == 0) ? &gGr2ExportClassDesc : nullptr;
}

__declspec(dllexport) ULONG LibVersion() {
    return VERSION_3DSMAX;
}

__declspec(dllexport) ULONG CanAutoDefer() {
    return 1;
}

GR2Export::GR2Export() = default;
GR2Export::~GR2Export() = default;

int GR2Export::ExtCount() {
    return 1;
}

const TCHAR* GR2Export::Ext(int n) {
    return (n == 0) ? _T("GR2") : _T("");
}

const TCHAR* GR2Export::LongDesc() {
    return GetString(IDS_GR2_FILE);
}

const TCHAR* GR2Export::ShortDesc() {
    return GetString(IDS_GR2_EXPORTER);
}

const TCHAR* GR2Export::AuthorName() {
    return GetString(IDS_AUTHOR);
}

const TCHAR* GR2Export::CopyrightMessage() {
    return GetString(IDS_COPYRIGHT);
}

const TCHAR* GR2Export::OtherMessage1() {
    return _T("");
}

const TCHAR* GR2Export::OtherMessage2() {
    return _T("");
}

unsigned int GR2Export::Version() {
    return 100;
}

void GR2Export::ShowAbout(HWND) {
}

BOOL GR2Export::SupportsOptions(int, DWORD options) {
    return (options == SCENE_EXPORT_SELECTED) ? TRUE : FALSE;
}

int GR2Export::DoExport(const TCHAR* name, ExpInterface*, Interface* ip, BOOL, DWORD options) {
    if (!GrannyVersionsMatch) {
        ShowError(L"Granny DLL/header version mismatch.");
        return IMPEXP_FAIL;
    }

    const bool exportSelected = (options & SCENE_EXPORT_SELECTED) != 0;
    std::vector<INode*> nodes;
    CollectExportNodes(ip->GetRootNode(), exportSelected, nodes);
    if (nodes.empty()) {
        ShowError(exportSelected ? L"No selected mesh objects were found to export." : L"No mesh objects were found to export.");
        return IMPEXP_FAIL;
    }

    ExportPackage package;
    const TimeValue time = ip->GetTime();
    int exportedMeshCount = 0;
    std::wstring firstError;

    for (INode* node : nodes) {
        std::wstring error;
        if (package.addNode(node, time, exportedMeshCount, error)) {
            ++exportedMeshCount;
        } else if (firstError.empty() && !error.empty()) {
            firstError = error;
        }
    }

    if (exportedMeshCount == 0) {
        ShowError(firstError.empty() ? L"The exporter could not convert any mesh objects." : firstError);
        return IMPEXP_FAIL;
    }

    std::wstring writeError;
    if (!package.writeToFile(ToWide(name), writeError)) {
        ShowError(writeError);
        return IMPEXP_FAIL;
    }

    return IMPEXP_SUCCESS;
}

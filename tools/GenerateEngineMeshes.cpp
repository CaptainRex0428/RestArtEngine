// Rebuild the checked-in Engine Content primitives. This is an offline CPU tool;
// normal engine builds and users never need to run it.
#include "RestArtEngine/Assets/AssetFile.h"
#include "RestArtEngine/Assets/AssetThumbnail.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using namespace rae::assets;
constexpr unsigned Segments = 96;
constexpr unsigned TubeSegments = 32;
constexpr double Pi = 3.14159265358979323846;
using Vec3 = std::array<float, 3>;

Vec3 subtract(const float* a, const float* b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
}
float dot(const Vec3& a, const Vec3& b) {
    return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
}
Vec3 normal(Vec3 n) {
    const float length = std::sqrt(dot(n,n));
    for (auto& component : n) component /= length;
    return n;
}
std::uint32_t vertex(MeshAsset& mesh, Vec3 p, Vec3 n) {
    const auto index = static_cast<std::uint32_t>(mesh.vertices.size());
    n = normal(n);
    mesh.vertices.push_back({{p[0],p[1],p[2]},{n[0],n[1],n[2]}});
    return index;
}
void triangle(MeshAsset& mesh, std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    const auto& va = mesh.vertices[a];
    const auto& vb = mesh.vertices[b];
    const auto& vc = mesh.vertices[c];
    const auto geometric = cross(subtract(vb.position,va.position), subtract(vc.position,va.position));
    const Vec3 outward{va.normal[0]+vb.normal[0]+vc.normal[0],
        va.normal[1]+vb.normal[1]+vc.normal[1], va.normal[2]+vb.normal[2]+vc.normal[2]};
    if (dot(geometric,outward) < 0) std::swap(b,c);
    mesh.indices.insert(mesh.indices.end(), {a,b,c});
}
Vec3 radial(unsigned segment, unsigned count = Segments) {
    // Wrap exactly, so duplicate seam vertices have identical coordinates.
    const double angle = 2*Pi*(segment % count)/count;
    return {static_cast<float>(std::cos(angle)),0,static_cast<float>(std::sin(angle))};
}
void cap(MeshAsset& mesh, float y, bool top) {
    const Vec3 n{0,top ? 1.0f : -1.0f,0};
    const auto center = vertex(mesh,{0,y,0},n);
    const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
    for (unsigned i=0; i<Segments; ++i) {
        const auto r = radial(i);
        vertex(mesh,{r[0],y,r[2]},n);
    }
    for (unsigned i=0; i<Segments; ++i)
        triangle(mesh,center,first+i,first+(i+1)%Segments);
}
MeshAsset cylinder() {
    MeshAsset mesh;
    mesh.id = EngineCylinderMeshId;
    mesh.name = "Cylinder";
    for (unsigned i=0; i<Segments; ++i) {
        const auto r = radial(i);
        vertex(mesh,{r[0],-1,r[2]},r);
        vertex(mesh,{r[0], 1,r[2]},r);
    }
    for (unsigned i=0; i<Segments; ++i) {
        const auto a = i*2, b = ((i+1)%Segments)*2;
        triangle(mesh,a,b,a+1);
        triangle(mesh,b,b+1,a+1);
    }
    cap(mesh,-1,false);
    cap(mesh,1,true);
    return mesh;
}
MeshAsset cone() {
    MeshAsset mesh;
    mesh.id = EngineConeMeshId;
    mesh.name = "Cone";
    for (unsigned i=0; i<Segments; ++i) {
        const auto r = radial(i);
        vertex(mesh,{r[0],-1,r[2]},{2*r[0],1,2*r[2]});
    }
    for (unsigned i=0; i<Segments; ++i) {
        // The apex is a singularity: each sector gets its own bisector normal.
        const auto r = radial(2*i+1,2*Segments);
        const auto apex = vertex(mesh,{0,1,0},{2*r[0],1,2*r[2]});
        triangle(mesh,i,(i+1)%Segments,apex);
    }
    cap(mesh,-1,false);
    return mesh;
}
MeshAsset torus() {
    MeshAsset mesh;
    mesh.id = EngineTorusMeshId;
    mesh.name = "Torus";
    for (unsigned ring=0; ring<Segments; ++ring) {
        const auto r = radial(ring);
        for (unsigned side=0; side<TubeSegments; ++side) {
            const auto tube = radial(side,TubeSegments);
            const float radius = .7f + .3f*tube[0];
            vertex(mesh,{radius*r[0],.3f*tube[2],radius*r[2]},
                {tube[0]*r[0],tube[2],tube[0]*r[2]});
        }
    }
    for (unsigned ring=0; ring<Segments; ++ring)
        for (unsigned side=0; side<TubeSegments; ++side) {
            const auto nextRing = (ring+1)%Segments, nextSide = (side+1)%TubeSegments;
            const auto a = ring*TubeSegments+side, b = nextRing*TubeSegments+side;
            const auto c = nextRing*TubeSegments+nextSide, d = ring*TubeSegments+nextSide;
            triangle(mesh,a,b,c);
            triangle(mesh,a,c,d);
        }
    return mesh;
}
void validate(const MeshAsset& mesh) {
    if (mesh.vertices.empty() || mesh.indices.empty() || mesh.indices.size()%3)
        throw std::runtime_error(mesh.name+": empty or incomplete mesh");
    for (const auto& v : mesh.vertices) {
        for (const auto p : v.position)
            if (!std::isfinite(p)) throw std::runtime_error(mesh.name+": non-finite position");
        const Vec3 n{v.normal[0],v.normal[1],v.normal[2]};
        if (!std::isfinite(dot(n,n)) || std::abs(dot(n,n)-1) > 1e-5f)
            throw std::runtime_error(mesh.name+": invalid unit normal");
    }
    for (std::size_t i=0; i<mesh.indices.size(); i+=3) {
        const auto& a = mesh.vertices.at(mesh.indices[i]);
        const auto& b = mesh.vertices.at(mesh.indices[i+1]);
        const auto& c = mesh.vertices.at(mesh.indices[i+2]);
        const auto n = cross(subtract(b.position,a.position), subtract(c.position,a.position));
        const Vec3 outward{a.normal[0]+b.normal[0]+c.normal[0],
            a.normal[1]+b.normal[1]+c.normal[1],a.normal[2]+b.normal[2]+c.normal[2]};
        if (dot(n,n) <= 1e-14f || dot(n,outward) <= 0)
            throw std::runtime_error(mesh.name+": degenerate or inward triangle");
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: GenerateEngineMeshes <output-directory>\n";
        return 2;
    }
    try {
        const auto output = std::filesystem::u8path(argv[1]);
        AssetLibrary defaults;
        defaults.createDefaults();
        auto cube = *defaults.findMesh(3);
        cube.id = EngineCubeMeshId;
        auto sphere = *defaults.findMesh(1);
        sphere.id = EngineSphereMeshId;
        std::vector<MeshAsset> meshes;
        meshes.push_back(std::move(cube));
        meshes.push_back(std::move(sphere));
        meshes.push_back(cylinder());
        meshes.push_back(cone());
        meshes.push_back(torus());
        auto plane = *defaults.findMesh(2);
        plane.id = EnginePlaneMeshId;
        meshes.push_back(std::move(plane));
        // Validate the complete set before publishing any output.
        for (const auto& mesh : meshes) validate(mesh);
        std::filesystem::create_directories(output);
        for (const auto& mesh : meshes) {
            auto file = makeMeshAssetFile(mesh);
            attachThumbnail(file,renderMeshThumbnail(mesh));
            std::string error;
            if (!writeAssetFile(output/(mesh.name+".rasset"),file,error))
                throw std::runtime_error(error);
            std::cout << mesh.name << ": " << mesh.vertices.size() << " vertices, "
                << mesh.indices.size()/3 << " triangles\n";
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

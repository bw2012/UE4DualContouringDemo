// Fill out your copyright notice in the Description page of Project Settings.

#include "FastDualContouringActor.h"
#include "DrawDebugHelpers.h"
#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include "qef_simd.h"
#include "VoxelIndex.h"

struct EdgeInfo {
	FVector4 pos;
	FVector4 normal;
	bool winding = false;
};

using EdgeInfoMap = std::unordered_map<uint32_t, EdgeInfo>;
using VoxelIDSet = std::unordered_set<uint32_t>;
using VoxelIndexMap = std::unordered_map<uint32_t, int>;


AFastDualContouringActor::AFastDualContouringActor() {
	PrimaryActorTick.bCanEverTick = true;

	Mesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("GeneratedMesh"));
	RootComponent = Mesh;
	Mesh->bUseAsyncCooking = true;
}

static const TVoxelIndex4 AXIS_OFFSET[3] = {
	TVoxelIndex4(1, 0, 0, 0),
	TVoxelIndex4(0, 1, 0, 0),
	TVoxelIndex4(0, 0, 1, 0)
};

static const TVoxelIndex4 EDGE_NODE_OFFSETS[3][4] = {
	{ TVoxelIndex4(0), TVoxelIndex4(0, 0, 1, 0), TVoxelIndex4(0, 1, 0, 0), TVoxelIndex4(0, 1, 1, 0) },
	{ TVoxelIndex4(0), TVoxelIndex4(1, 0, 0, 0), TVoxelIndex4(0, 0, 1, 0), TVoxelIndex4(1, 0, 1, 0) },
	{ TVoxelIndex4(0), TVoxelIndex4(0, 1, 0, 0), TVoxelIndex4(1, 0, 0, 0), TVoxelIndex4(1, 1, 0, 0) },
};

const uint32 ENCODED_EDGE_OFFSETS[12] = {
	0x00000000,
	0x00100000,
	0x00000400,
	0x00100400,
	0x40000000,
	0x40100000,
	0x40000001,
	0x40100001,
	0x80000000,
	0x80000400,
	0x80000001,
	0x80000401,
};

const uint32_t ENCODED_EDGE_NODE_OFFSETS[12] = {
	0x00000000,
	0x00100000,
	0x00000400,
	0x00100400,
	0x00000000,
	0x00000001,
	0x00100000,
	0x00100001,
	0x00000000,
	0x00000400,
	0x00000001,
	0x00000401,
};


float Density(const TVoxelData* VoxelData, const TVoxelIndex4& Index) {
	return VoxelData->getDensity(Index.X, Index.Y, Index.Z);
}

// returns x * (1.0 - a) + y * a 
// the linear blend of x and y using the floating-point value a
FVector4 mix(const FVector4& x, const FVector4& y, float a) {
	return x * (1.f - a) + y * a;
}

FVector vertexInterpolation(FVector p1, FVector p2, float valp1, float valp2) {
	static const float isolevel = 0.5f;

	if (std::abs(isolevel - valp1) < 0.00001) {
		return p1;
	}

	if (std::abs(isolevel - valp2) < 0.00001) {
		return p2;
	}

	if (std::abs(valp1 - valp2) < 0.00001) {
		return p1;
	}

	if (valp1 == valp2) {
		return p1;
	}

	float mu = (isolevel - valp1) / (valp2 - valp1);
	return p1 + (p2 - p1) *mu;
}

uint32 EncodeAxisUniqueID(const int axis, const int x, const int y, const int z) {
	return (x << 0) | (y << 10) | (z << 20) | (axis << 30);
}

uint32 EncodeVoxelUniqueID(const TVoxelIndex4& idxPos) {
	return idxPos.X | (idxPos.Y << 10) | (idxPos.Z << 20);
}

TVoxelIndex4 DecodeVoxelUniqueID(const uint32_t id) {
	return TVoxelIndex4(id & 0x3ff, (id >> 10) & 0x3ff,	(id >> 20) & 0x3ff,	0);
}

void FindActiveVoxels(const TVoxelData* voxelData, VoxelIDSet& activeVoxels, EdgeInfoMap& activeEdges) {
	for (int x = 0; x < voxelData->num(); x++) {
		for (int y = 0; y < voxelData->num(); y++) {
			for (int z = 0; z < voxelData->num(); z++) {
				const TVoxelIndex4 p(x, y, z, 0);

				for (int axis = 0; axis < 3; axis++) {
					const TVoxelIndex4 q = p + AXIS_OFFSET[axis];

					const float pDensity = Density(voxelData, p);
					const float qDensity = Density(voxelData, q);

					const bool zeroCrossing = (pDensity >= 0.5f && qDensity < 0.5f) || (pDensity < 0.5f && qDensity >= 0.5f);

					if (!zeroCrossing) continue;

					//UE_LOG(LogTemp, Warning, TEXT("x, y, z - pDensity, qDensity  --> %d %d %d - %f %f"), x, y, z, pDensity, qDensity);

					const FVector p1 = voxelData->voxelIndexToVector(p.X, p.Y, p.Z);
					const FVector q1 = voxelData->voxelIndexToVector(q.X, q.Y, q.Z);
					const FVector4 pos = vertexInterpolation(p1, q1, pDensity, qDensity);

					FVector4 tmp(
						Density(voxelData, p + TVoxelIndex4(1, 0, 0, 0)) - Density(voxelData, p - TVoxelIndex4(1, 0, 0, 0)),
						Density(voxelData, p + TVoxelIndex4(0, 1, 0, 0)) - Density(voxelData, p - TVoxelIndex4(0, 1, 0, 0)),
						Density(voxelData, p + TVoxelIndex4(0, 0, 1, 0)) - Density(voxelData, p - TVoxelIndex4(0, 0, 1, 0)),
						0.f);

					auto normal = -tmp.GetSafeNormal(0.000001f);

					EdgeInfo info;
					info.pos = pos;
					info.normal = normal;
					info.winding = pDensity >= 0.5f;

					const auto code = EncodeAxisUniqueID(axis, x, y, z);

					//UE_LOG(LogTemp, Warning, TEXT("code  --> %d"), code);

					activeEdges[code] = info;

					const auto edgeNodes = EDGE_NODE_OFFSETS[axis];
					for (int i = 0; i < 4; i++) {
						const auto nodeIdxPos = p - edgeNodes[i];
						const auto nodeID = EncodeVoxelUniqueID(nodeIdxPos);
						activeVoxels.insert(nodeID);
					}
				}
			}
		}
	}
}

static void GenerateVertexData(const VoxelIDSet& voxels, const EdgeInfoMap& edges, VoxelIndexMap& vertexIndices, TArray<FVector>& varray, TArray<FVector>& narray) {
	int idxCounter = 0;
	for (const auto& voxelID : voxels) {
		FVector4 p[12];
		FVector4 n[12];

		int idx = 0;
		for (int i = 0; i < 12; i++) {
			const auto edgeID = voxelID + ENCODED_EDGE_OFFSETS[i];
			const auto iter = edges.find(edgeID);

			if (iter != end(edges)) {
				const auto& info = iter->second;
				const FVector4 pos = info.pos;
				const FVector4 normal = info.normal;

				p[idx] = pos;
				n[idx] = normal;
				idx++;
			}
		}

		FVector4 nodePos;
		qef_solve_from_points_4d(&p[0].X, &n[0].X, idx, &nodePos.X);

		FVector4 nodeNormal;
		for (int i = 0; i < idx; i++) {
			nodeNormal += n[i];
		}

		nodeNormal *= (1.f / (float)idx);

		vertexIndices[voxelID] = idxCounter++;
		varray.Add(FVector(nodePos.X, nodePos.Y, nodePos.Z));
		narray.Add(FVector(nodeNormal.X, nodeNormal.Y, nodeNormal.Z));
	}

}

static void GenerateTriangles(const EdgeInfoMap& edges, const VoxelIndexMap& vertexIndices, TArray<int32>& triarray) {
	for (const auto& pair : edges) {
		const auto& edge = pair.first;
		const auto& info = pair.second;

		const TVoxelIndex4 basePos = DecodeVoxelUniqueID(edge);
		const int axis = (edge >> 30) & 0xff;

		const int nodeID = edge & ~0xc0000000;
		const uint32_t voxelIDs[4] = {
			nodeID - ENCODED_EDGE_NODE_OFFSETS[axis * 4 + 0],
			nodeID - ENCODED_EDGE_NODE_OFFSETS[axis * 4 + 1],
			nodeID - ENCODED_EDGE_NODE_OFFSETS[axis * 4 + 2],
			nodeID - ENCODED_EDGE_NODE_OFFSETS[axis * 4 + 3],
		};

		// attempt to find the 4 voxels which share this edge
		int edgeVoxels[4];
		int numFoundVoxels = 0;
		for (int i = 0; i < 4; i++) {
			const auto iter = vertexIndices.find(voxelIDs[i]);
			if (iter != end(vertexIndices)) {
				edgeVoxels[numFoundVoxels++] = iter->second;
			}
		}

		// we can only generate a quad (or two triangles) if all 4 are found
		if (numFoundVoxels < 4) {
			continue;
		}

		if (info.winding) {
			triarray.Add(edgeVoxels[0]);
			triarray.Add(edgeVoxels[1]);
			triarray.Add(edgeVoxels[3]);

			triarray.Add(edgeVoxels[0]);
			triarray.Add(edgeVoxels[3]);
			triarray.Add(edgeVoxels[2]);
		} else {
			triarray.Add(edgeVoxels[0]);
			triarray.Add(edgeVoxels[3]);
			triarray.Add(edgeVoxels[1]);

			triarray.Add(edgeVoxels[0]);
			triarray.Add(edgeVoxels[2]);
			triarray.Add(edgeVoxels[3]);
		}
	}
}


void AFastDualContouringActor::BeginPlay() {
	Super::BeginPlay();

	VoxelData = new TVoxelData(256, 500);

	static const float Extend = 100.f;

	VoxelData->forEach([&](int x, int y, int z) {
		FVector Pos = VoxelData->voxelIndexToVector(x, y, z);
		if (Pos.X < Extend && Pos.X > -Extend && Pos.Y < Extend && Pos.Y > -Extend && Pos.Z < Extend && Pos.Z > -Extend) {
			VoxelData->setDensity(x, y, z, 1);
			//DrawDebugPoint(GetWorld(), Pos + GetActorLocation(), 3, FColor(255, 0, 0), true, 10000000);
		}
	});

	FVector Pos(100, 100, 100);
	static const float R = 50.f;
	static const float Extend2 = R * 5.f;

	VoxelData->forEach([&](int x, int y, int z) {
		float density = VoxelData->getDensity(x, y, z);
		FVector o = VoxelData->voxelIndexToVector(x, y, z);
		o -= Pos;

		float rl = std::sqrt(o.X * o.X + o.Y * o.Y + o.Z * o.Z);
		if (rl < Extend2) {
			float d = density + 1 / rl * R;
			VoxelData->setDensity(x, y, z, d);
		}

	});

	VoxelIDSet activeVoxels;
	EdgeInfoMap activeEdges;

	FindActiveVoxels(VoxelData, activeVoxels, activeEdges);

	UE_LOG(LogTemp, Warning, TEXT("activeVoxels --> %d"), activeVoxels.size());
	UE_LOG(LogTemp, Warning, TEXT("activeEdges  --> %d"), activeEdges.size());


	TArray<FVector> varray;
	TArray<FVector> narray;
	TArray<int32> triarray;
	VoxelIndexMap vertexIndices;

	GenerateVertexData(activeVoxels, activeEdges, vertexIndices, varray, narray);

	UE_LOG(LogTemp, Warning, TEXT("varray --> %d"), varray.Num());
	UE_LOG(LogTemp, Warning, TEXT("narray  --> %d"), narray.Num());
	UE_LOG(LogTemp, Warning, TEXT("vertexIndices  --> %d"), vertexIndices.size());

	GenerateTriangles(activeEdges, vertexIndices, triarray);

	UE_LOG(LogTemp, Warning, TEXT("triarray  --> %d"), triarray.Num());

	TArray<FVector2D> UV0;
	TArray<FLinearColor> vertexColors;
	TArray<FProcMeshTangent> tangents;

	Mesh->CreateMeshSection_LinearColor(0, varray, triarray, narray, UV0, vertexColors, tangents, true);
	Mesh->bUseAsyncCooking = true;
	Mesh->SetMaterial(0, Material);

	/*
	TArray<FVector> vertices;
	vertices.Add(FVector(0, 0, 0));
	vertices.Add(FVector(0, 100, 0));
	vertices.Add(FVector(0, 0, 100));

	TArray<int32> Triangles;
	Triangles.Add(0);
	Triangles.Add(1);
	Triangles.Add(2);

	TArray<FVector> normals;
	normals.Add(FVector(1, 0, 0));
	normals.Add(FVector(1, 0, 0));
	normals.Add(FVector(1, 0, 0));

	TArray<FVector2D> UV0;
	UV0.Add(FVector2D(0, 0));
	UV0.Add(FVector2D(10, 0));
	UV0.Add(FVector2D(0, 10));


	TArray<FProcMeshTangent> tangents;
	tangents.Add(FProcMeshTangent(0, 1, 0));
	tangents.Add(FProcMeshTangent(0, 1, 0));
	tangents.Add(FProcMeshTangent(0, 1, 0));

	TArray<FLinearColor> vertexColors;
	vertexColors.Add(FLinearColor(0.75, 0.75, 0.75, 1.0));
	vertexColors.Add(FLinearColor(0.75, 0.75, 0.75, 1.0));
	vertexColors.Add(FLinearColor(0.75, 0.75, 0.75, 1.0));

	mesh->CreateMeshSection_LinearColor(0, vertices, Triangles, normals, UV0, vertexColors, tangents, true);

	// Enable collision data
	mesh->ContainsPhysicsTriMeshData(true);
	*/
}


void AFastDualContouringActor::Tick(float DeltaTime) {
	Super::Tick(DeltaTime);

}



//====================================================================================
// Voxel data impl
//====================================================================================

TVoxelData::TVoxelData(int num, float size) {
	// int s = num*num*num;

	density_data = NULL;
	density_state = TVoxelDataFillState::ZERO;

	material_data = NULL;

	voxel_num = num;
	volume_size = size;

	UE_LOG(LogTemp, Warning, TEXT("num  --> %d "), num);
}

TVoxelData::~TVoxelData() {
	delete[] density_data;
	delete[] material_data;
}

FORCEINLINE void TVoxelData::initializeDensity() {
	int s = voxel_num * voxel_num * voxel_num;
	density_data = new unsigned char[s];
	for (auto x = 0; x < voxel_num; x++) {
		for (auto y = 0; y < voxel_num; y++) {
			for (auto z = 0; z < voxel_num; z++) {
				if (density_state == TVoxelDataFillState::ALL) {
					setDensity(x, y, z, 1);
				}

				if (density_state == TVoxelDataFillState::ZERO) {
					setDensity(x, y, z, 0);
				}
			}
		}
	}
}

FORCEINLINE void TVoxelData::initializeMaterial() {
	int s = voxel_num * voxel_num * voxel_num;
	material_data = new unsigned short[s];
	for (auto x = 0; x < voxel_num; x++) {
		for (auto y = 0; y < voxel_num; y++) {
			for (auto z = 0; z < voxel_num; z++) {
				setMaterial(x, y, z, base_fill_mat);
			}
		}
	}
}

FORCEINLINE void TVoxelData::setDensity(int x, int y, int z, float density) {
	if (density_data == NULL) {
		if (density_state == TVoxelDataFillState::ZERO && density == 0) {
			return;
		}

		if (density_state == TVoxelDataFillState::ALL && density == 1) {
			return;
		}

		initializeDensity();
		density_state = TVoxelDataFillState::MIX;
	}

	if (x < voxel_num && y < voxel_num && z < voxel_num) {
		int index = x * voxel_num * voxel_num + y * voxel_num + z;

		if (density < 0) density = 0;
		if (density > 1) density = 1;

		unsigned char d = 255 * density;

		density_data[index] = d;
	}
}

FORCEINLINE float TVoxelData::getDensity(int x, int y, int z) const {
	if (density_data == NULL) {
		if (density_state == TVoxelDataFillState::ALL) {
			return 1;
		}

		return 0;
	}

	if (x < voxel_num && y < voxel_num && z < voxel_num) {
		int index = x * voxel_num * voxel_num + y * voxel_num + z;

		float d = (float)density_data[index] / 255.0f;
		return d;
	}
	else {
		return 0;
	}
}

FORCEINLINE unsigned char TVoxelData::getRawDensity(int x, int y, int z) const {
	auto index = x * voxel_num * voxel_num + y * voxel_num + z;
	return density_data[index];
}

FORCEINLINE void TVoxelData::setMaterial(const int x, const int y, const int z, const unsigned short material) {
	if (material_data == NULL) {
		initializeMaterial();
	}

	if (x < voxel_num && y < voxel_num && z < voxel_num) {
		int index = x * voxel_num * voxel_num + y * voxel_num + z;
		material_data[index] = material;
	}
}

FORCEINLINE unsigned short TVoxelData::getMaterial(int x, int y, int z) const {
	if (material_data == NULL) {
		return base_fill_mat;
	}

	if (x < voxel_num && y < voxel_num && z < voxel_num) {
		int index = x * voxel_num * voxel_num + y * voxel_num + z;
		return material_data[index];
	}
	else {
		return 0;
	}
}

FORCEINLINE FVector TVoxelData::voxelIndexToVector(int x, int y, int z) const {
	static const float step = size() / (num() - 1);
	static const float s = -size() / 2;
	FVector v(s, s, s);
	FVector a(x * step, y * step, z * step);
	v = v + a;
	return v;
}

void TVoxelData::vectorToVoxelIndex(const FVector& v, int& x, int& y, int& z) const {
	static const float step = size() / (num() - 1);

	x = (int)(v.X / step) + num() / 2 - 1;
	y = (int)(v.Y / step) + num() / 2 - 1;
	z = (int)(v.Z / step) + num() / 2 - 1;
}

void TVoxelData::setOrigin(FVector o) {
	origin = o;
	lower = FVector(o.X - volume_size, o.Y - volume_size, o.Z - volume_size);
	upper = FVector(o.X + volume_size, o.Y + volume_size, o.Z + volume_size);
}

FORCEINLINE FVector TVoxelData::getOrigin() const {
	return origin;
}

FORCEINLINE float TVoxelData::size() const {
	return volume_size;
}

FORCEINLINE int TVoxelData::num() const {
	return voxel_num;
}

FORCEINLINE TVoxelPoint TVoxelData::getVoxelPoint(int x, int y, int z) const {
	TVoxelPoint vp;
	int index = x * voxel_num * voxel_num + y * voxel_num + z;

	vp.material = base_fill_mat;
	vp.density = 0;

	if (density_data != NULL) {
		vp.density = density_data[index];
	}

	if (material_data != NULL) {
		vp.material = material_data[index];
	}

	return vp;
}

FORCEINLINE void TVoxelData::setVoxelPoint(int x, int y, int z, unsigned char density, unsigned short material) {
	if (density_data == NULL) {
		initializeDensity();
		density_state = TVoxelDataFillState::MIX;
	}

	if (material_data == NULL) {
		initializeMaterial();
	}

	int index = x * voxel_num * voxel_num + y * voxel_num + z;
	material_data[index] = material;
	density_data[index] = density;
}

FORCEINLINE void TVoxelData::setVoxelPointDensity(int x, int y, int z, unsigned char density) {
	if (density_data == NULL) {
		initializeDensity();
		density_state = TVoxelDataFillState::MIX;
	}

	int index = x * voxel_num * voxel_num + y * voxel_num + z;
	density_data[index] = density;
}

FORCEINLINE void TVoxelData::setVoxelPointMaterial(int x, int y, int z, unsigned short material) {
	if (material_data == NULL) {
		initializeMaterial();
	}

	int index = x * voxel_num * voxel_num + y * voxel_num + z;
	material_data[index] = material;
}

FORCEINLINE void TVoxelData::deinitializeDensity(TVoxelDataFillState State) {
	if (State == TVoxelDataFillState::MIX) {
		return;
	}

	density_state = State;
	if (density_data != NULL) {
		delete density_data;
	}

	density_data = NULL;
}

FORCEINLINE void TVoxelData::deinitializeMaterial(unsigned short base_mat) {
	base_fill_mat = base_mat;

	if (material_data != NULL) {
		delete material_data;
	}

	material_data = NULL;
}

FORCEINLINE TVoxelDataFillState TVoxelData::getDensityFillState()	const {
	return density_state;
}

FORCEINLINE bool TVoxelData::performCellSubstanceCaching(int x, int y, int z, int lod, int step) {
	if (x <= 0 || y <= 0 || z <= 0) {
		return false;
	}

	if (x < step || y < step || z < step) {
		return false;
	}

	unsigned char density[8];
	static unsigned char isolevel = 127;

	const int rx = x - step;
	const int ry = y - step;
	const int rz = z - step;

	density[0] = getRawDensity(x, y - step, z);
	density[1] = getRawDensity(x, y, z);
	density[2] = getRawDensity(x - step, y - step, z);
	density[3] = getRawDensity(x - step, y, z);
	density[4] = getRawDensity(x, y - step, z - step);
	density[5] = getRawDensity(x, y, z - step);
	density[6] = getRawDensity(rx, ry, rz);
	density[7] = getRawDensity(x - step, y, z - step);

	if (density[0] > isolevel &&
		density[1] > isolevel &&
		density[2] > isolevel &&
		density[3] > isolevel &&
		density[4] > isolevel &&
		density[5] > isolevel &&
		density[6] > isolevel &&
		density[7] > isolevel) {
		return false;
	}

	if (density[0] <= isolevel &&
		density[1] <= isolevel &&
		density[2] <= isolevel &&
		density[3] <= isolevel &&
		density[4] <= isolevel &&
		density[5] <= isolevel &&
		density[6] <= isolevel &&
		density[7] <= isolevel) {
		return false;
	}

	int index = clcLinearIndex(rx, ry, rz);
	TSubstanceCache& lodCache = substanceCacheLOD[lod];
	lodCache.cellList.push_back(index);
	return true;
}


FORCEINLINE void TVoxelData::performSubstanceCacheNoLOD(int x, int y, int z) {
	if (density_data == NULL) {
		return;
	}

	performCellSubstanceCaching(x, y, z, 0, 1);
}

FORCEINLINE void TVoxelData::performSubstanceCacheLOD(int x, int y, int z) {
	if (density_data == NULL) {
		return;
	}

	for (auto lod = 0; lod < LOD_ARRAY_SIZE; lod++) {
		int s = 1 << lod;
		if (x >= s && y >= s && z >= s) {
			if (x % s == 0 && y % s == 0 && z % s == 0) {
				performCellSubstanceCaching(x, y, z, lod, s);
			}
		}
	}
}

void TVoxelData::forEach(std::function<void(int x, int y, int z)> func) {
	for (int x = 0; x < num(); x++)
		for (int y = 0; y < num(); y++)
			for (int z = 0; z < num(); z++)
				func(x, y, z);
}

void TVoxelData::forEachWithCache(std::function<void(int x, int y, int z)> func, bool LOD) {
	clearSubstanceCache();

	for (int x = 0; x < num(); x++) {
		for (int y = 0; y < num(); y++) {
			for (int z = 0; z < num(); z++) {
				func(x, y, z);

				if (LOD) {
					performSubstanceCacheLOD(x, y, z);
				}
				else {
					performSubstanceCacheNoLOD(x, y, z);
				}

			}
		}
	}
}


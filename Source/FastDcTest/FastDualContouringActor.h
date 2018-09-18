// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralMeshComponent.h"
#include <memory>
#include <list>
#include <array>
#include <functional>
#include "FastDualContouringActor.generated.h"


#define LOD_ARRAY_SIZE 7

typedef struct TVoxelPoint {
	unsigned char density;
	unsigned short material;
} TVoxelPoint;


typedef struct TVoxelCell {
	TVoxelPoint point[8];
} TVoxelCell;


enum TVoxelDataFillState {
	ZERO, ALL, MIX
};

typedef struct TSubstanceCache {
	std::list<int> cellList;
} TSubstanceCache;

class TVoxelData {

private:
	TVoxelDataFillState density_state;
	unsigned short base_fill_mat = 0;

	int voxel_num;
	float volume_size;
	unsigned char* density_data;
	unsigned short* material_data;

	volatile double last_change;
	volatile double last_save;
	volatile double last_mesh_generation;
	volatile double last_cache_check;

	FVector origin = FVector(0.0f, 0.0f, 0.0f);
	FVector lower = FVector(0.0f, 0.0f, 0.0f);
	FVector upper = FVector(0.0f, 0.0f, 0.0f);

	void initializeDensity();
	void initializeMaterial();

	bool performCellSubstanceCaching(int x, int y, int z, int lod, int step);

public:
	std::array<TSubstanceCache, LOD_ARRAY_SIZE> substanceCacheLOD;

	TVoxelData(int, float);
	~TVoxelData();

	FORCEINLINE int clcLinearIndex(int x, int y, int z) const {
		return x * voxel_num * voxel_num + y * voxel_num + z;
	};

	void forEach(std::function<void(int x, int y, int z)> func);
	void forEachWithCache(std::function<void(int x, int y, int z)> func, bool enableLOD);

	void setDensity(int x, int y, int z, float density);
	float getDensity(int x, int y, int z) const;
	unsigned char getRawDensity(int x, int y, int z) const;

	void setMaterial(const int x, const int y, const int z, unsigned short material);
	unsigned short getMaterial(int x, int y, int z) const;

	float size() const;
	int num() const;

	FVector voxelIndexToVector(int x, int y, int z) const;
	void vectorToVoxelIndex(const FVector& v, int& x, int& y, int& z) const;

	void setOrigin(FVector o);
	FVector getOrigin() const;

	FVector getLower() const { return lower; };
	FVector getUpper() const { return upper; };

	TVoxelPoint getVoxelPoint(int x, int y, int z) const;
	void setVoxelPoint(int x, int y, int z, unsigned char density, unsigned short material);
	void setVoxelPointDensity(int x, int y, int z, unsigned char density);
	void setVoxelPointMaterial(int x, int y, int z, unsigned short material);

	void performSubstanceCacheNoLOD(int x, int y, int z);
	void performSubstanceCacheLOD(int x, int y, int z);

	TVoxelDataFillState getDensityFillState() const;
	//VoxelDataFillState getMaterialFillState() const; 

	void deinitializeDensity(TVoxelDataFillState density_state);
	void deinitializeMaterial(unsigned short base_mat);

	void setChanged() { last_change = FPlatformTime::Seconds(); }
	bool isChanged() { return last_change > last_save; }
	void resetLastSave() { last_save = FPlatformTime::Seconds(); }
	bool needToRegenerateMesh() { return last_change > last_mesh_generation; }
	void resetLastMeshRegenerationTime() { last_mesh_generation = FPlatformTime::Seconds(); }

	bool isSubstanceCacheValid() const { return last_change <= last_cache_check; }
	void setCacheToValid() { last_cache_check = FPlatformTime::Seconds(); }

	void clearSubstanceCache() {
		for (TSubstanceCache& lodCache : substanceCacheLOD) {
			lodCache.cellList.clear();
		}

		last_cache_check = -1;
	};

};


UCLASS()
class FASTDCTEST_API AFastDualContouringActor : public AActor
{
	GENERATED_BODY()
	
public:	
	// Sets default values for this actor's properties
	AFastDualContouringActor();

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

public:	
	// Called every frame
	virtual void Tick(float DeltaTime) override;

	
private:

	UPROPERTY(VisibleAnywhere)
	UProceduralMeshComponent* Mesh;

	UPROPERTY(EditAnywhere)
	UMaterial* Material;

protected:
	TVoxelData* VoxelData;
	
};

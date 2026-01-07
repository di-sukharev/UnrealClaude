// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_AssetDependencies.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

FMCPToolResult FMCPTool_AssetDependencies::Execute(const TSharedRef<FJsonObject>& Params)
{
	// Extract required asset_path parameter
	FString AssetPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return Error.GetValue();
	}

	// Extract optional include_soft parameter
	bool bIncludeSoft = ExtractOptionalBool(Params, TEXT("include_soft"), true);

	// Get AssetRegistry
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	// Normalize the asset path - handle both package paths and full object paths
	FString PackagePath = AssetPath;
	if (PackagePath.Contains(TEXT(".")))
	{
		// Extract package path from full object path (e.g., /Game/BP.BP_C -> /Game/BP)
		PackagePath = FPackageName::ObjectPathToPackageName(AssetPath);
	}

	// Verify the asset exists
	FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
	if (!AssetData.IsValid())
	{
		// Try with package name
		TArray<FAssetData> AssetsInPackage;
		AssetRegistry.GetAssetsByPackageName(FName(*PackagePath), AssetsInPackage);
		if (AssetsInPackage.Num() == 0)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
		}
		AssetData = AssetsInPackage[0];
	}

	// Query dependencies
	TArray<FName> Dependencies;

	// Build dependency query flags - construct FDependencyQuery from EDependencyQuery enum
	UE::AssetRegistry::FDependencyQuery QueryFlags;
	if (!bIncludeSoft)
	{
		// Only hard dependencies
		QueryFlags = UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Hard);
	}
	// else: default FDependencyQuery() returns all dependencies (no requirements)

	AssetRegistry.GetDependencies(
		FName(*PackagePath),
		Dependencies,
		UE::AssetRegistry::EDependencyCategory::Package,
		QueryFlags
	);

	// Build result array
	TArray<TSharedPtr<FJsonValue>> DependencyArray;
	for (const FName& DepPath : Dependencies)
	{
		// Skip engine/script packages
		FString PathStr = DepPath.ToString();
		if (PathStr.StartsWith(TEXT("/Script/")) || PathStr.StartsWith(TEXT("/Engine/")))
		{
			continue;
		}

		TSharedPtr<FJsonObject> DepJson = MakeShared<FJsonObject>();
		DepJson->SetStringField(TEXT("path"), PathStr);

		// Try to get the asset class for this dependency
		TArray<FAssetData> DepAssets;
		AssetRegistry.GetAssetsByPackageName(DepPath, DepAssets);
		if (DepAssets.Num() > 0)
		{
			DepJson->SetStringField(TEXT("class"), DepAssets[0].AssetClassPath.GetAssetName().ToString());
			DepJson->SetStringField(TEXT("name"), DepAssets[0].AssetName.ToString());
		}

		DependencyArray.Add(MakeShared<FJsonValueObject>(DepJson));
	}

	// Build result data
	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("asset_path"), AssetPath);
	ResultData->SetArrayField(TEXT("dependencies"), DependencyArray);
	ResultData->SetNumberField(TEXT("count"), DependencyArray.Num());
	ResultData->SetBoolField(TEXT("include_soft"), bIncludeSoft);

	// Build message
	FString Message = FString::Printf(TEXT("Found %d dependenc%s for '%s'"),
		DependencyArray.Num(),
		DependencyArray.Num() == 1 ? TEXT("y") : TEXT("ies"),
		*AssetData.AssetName.ToString());

	return FMCPToolResult::Success(Message, ResultData);
}

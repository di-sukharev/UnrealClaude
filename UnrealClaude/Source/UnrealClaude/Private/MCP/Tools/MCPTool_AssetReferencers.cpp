// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_AssetReferencers.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

FMCPToolResult FMCPTool_AssetReferencers::Execute(const TSharedRef<FJsonObject>& Params)
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

	// Query referencers
	TArray<FName> Referencers;

	// Build dependency query flags - construct FDependencyQuery from EDependencyQuery enum
	UE::AssetRegistry::FDependencyQuery QueryFlags;
	if (!bIncludeSoft)
	{
		// Only hard references
		QueryFlags = UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Hard);
	}
	// else: default FDependencyQuery() returns all references (no requirements)

	AssetRegistry.GetReferencers(
		FName(*PackagePath),
		Referencers,
		UE::AssetRegistry::EDependencyCategory::Package,
		QueryFlags
	);

	// Build result array
	TArray<TSharedPtr<FJsonValue>> ReferencerArray;
	for (const FName& RefPath : Referencers)
	{
		// Skip engine/script packages
		FString PathStr = RefPath.ToString();
		if (PathStr.StartsWith(TEXT("/Script/")) || PathStr.StartsWith(TEXT("/Engine/")))
		{
			continue;
		}

		TSharedPtr<FJsonObject> RefJson = MakeShared<FJsonObject>();
		RefJson->SetStringField(TEXT("path"), PathStr);

		// Try to get the asset class for this referencer
		TArray<FAssetData> RefAssets;
		AssetRegistry.GetAssetsByPackageName(RefPath, RefAssets);
		if (RefAssets.Num() > 0)
		{
			RefJson->SetStringField(TEXT("class"), RefAssets[0].AssetClassPath.GetAssetName().ToString());
			RefJson->SetStringField(TEXT("name"), RefAssets[0].AssetName.ToString());
		}

		ReferencerArray.Add(MakeShared<FJsonValueObject>(RefJson));
	}

	// Build result data
	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("asset_path"), AssetPath);
	ResultData->SetArrayField(TEXT("referencers"), ReferencerArray);
	ResultData->SetNumberField(TEXT("count"), ReferencerArray.Num());
	ResultData->SetBoolField(TEXT("include_soft"), bIncludeSoft);

	// Build message
	FString Message;
	if (ReferencerArray.Num() == 0)
	{
		Message = FString::Printf(TEXT("No referencers found for '%s' - this asset appears unused"),
			*AssetData.AssetName.ToString());
	}
	else
	{
		Message = FString::Printf(TEXT("Found %d referencer%s for '%s'"),
			ReferencerArray.Num(),
			ReferencerArray.Num() == 1 ? TEXT("") : TEXT("s"),
			*AssetData.AssetName.ToString());
	}

	return FMCPToolResult::Success(Message, ResultData);
}

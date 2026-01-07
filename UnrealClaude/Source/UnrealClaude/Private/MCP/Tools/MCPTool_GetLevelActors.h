// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "../MCPToolBase.h"

/**
 * MCP Tool: Get all actors in the current level
 */
class FMCPTool_GetLevelActors : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("get_level_actors");
		Info.Description = TEXT(
			"Query actors in the current level with optional filtering.\n\n"
			"Use this tool to discover what's in the scene before making modifications. "
			"Results include actor name, label, class, and transform (location/rotation/scale).\n\n"
			"Filter examples:\n"
			"- class_filter='PointLight' - Find all point lights\n"
			"- class_filter='StaticMeshActor' - Find all static meshes\n"
			"- name_filter='Player' - Find actors with 'Player' in name\n\n"
			"Returns: Array of actors with name, label, class, location, rotation, scale. "
			"Use offset/limit for pagination on large levels."
		);
		Info.Parameters = {
			FMCPToolParameter(TEXT("class_filter"), TEXT("string"), TEXT("Optional class name to filter actors (e.g., 'StaticMeshActor', 'PointLight')"), false),
			FMCPToolParameter(TEXT("name_filter"), TEXT("string"), TEXT("Optional substring to filter actors by name"), false),
			FMCPToolParameter(TEXT("include_hidden"), TEXT("boolean"), TEXT("Include hidden actors in results"), false, TEXT("false")),
			FMCPToolParameter(TEXT("limit"), TEXT("number"), TEXT("Maximum number of actors to return (default: 100)"), false, TEXT("100")),
			FMCPToolParameter(TEXT("offset"), TEXT("number"), TEXT("Number of actors to skip for pagination"), false, TEXT("0"))
		};
		Info.Annotations = FMCPToolAnnotations::ReadOnly();
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;
};

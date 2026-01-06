// Copyright Your Name. All Rights Reserved.

#include "AnimGraphEditor.h"
#include "AnimStateMachineEditor.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_Root.h"
#include "AnimStateNode.h"
#include "AnimStateNodeBase.h"
#include "AnimStateTransitionNode.h"
#include "AnimationGraph.h"
#include "AnimationStateGraph.h"
#include "AnimationTransitionGraph.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_BlendSpacePlayer.h"
#include "AnimGraphNode_TransitionResult.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimGraphNode_Base.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/AnimMontage.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_TransitionRuleGetter.h"
#include "KismetCompiler.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet/KismetMathLibrary.h"
#include "HAL/PlatformAtomics.h"

volatile int32 FAnimGraphEditor::NodeIdCounter = 0;
const FString FAnimGraphEditor::NodeIdPrefix = TEXT("MCP_ANIM_ID:");

// ===== Graph Finding =====

UEdGraph* FAnimGraphEditor::FindAnimGraph(UAnimBlueprint* AnimBP, FString& OutError)
{
	if (!AnimBP)
	{
		OutError = TEXT("Invalid Animation Blueprint");
		return nullptr;
	}

	TArray<UEdGraph*> AllGraphs;
	AnimBP->GetAllGraphs(AllGraphs);

	for (UEdGraph* Graph : AllGraphs)
	{
		if (Graph->IsA<UAnimationGraph>())
		{
			return Graph;
		}
	}

	OutError = TEXT("Animation Blueprint has no AnimGraph");
	return nullptr;
}

UEdGraph* FAnimGraphEditor::FindStateBoundGraph(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	const FString& StateName,
	FString& OutError)
{
	// Find state machine
	UAnimGraphNode_StateMachine* StateMachine = FAnimStateMachineEditor::FindStateMachine(AnimBP, StateMachineName, OutError);
	if (!StateMachine) return nullptr;

	// Find state
	UAnimStateNode* State = FAnimStateMachineEditor::FindState(StateMachine, StateName, OutError);
	if (!State) return nullptr;

	// Get bound graph
	return FAnimStateMachineEditor::GetStateBoundGraph(State, OutError);
}

UEdGraph* FAnimGraphEditor::FindTransitionGraph(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	const FString& FromState,
	const FString& ToState,
	FString& OutError)
{
	// Find state machine
	UAnimGraphNode_StateMachine* StateMachine = FAnimStateMachineEditor::FindStateMachine(AnimBP, StateMachineName, OutError);
	if (!StateMachine) return nullptr;

	// Find transition
	UAnimStateTransitionNode* Transition = FAnimStateMachineEditor::FindTransition(StateMachine, FromState, ToState, OutError);
	if (!Transition) return nullptr;

	// Get transition graph
	return FAnimStateMachineEditor::GetTransitionGraph(Transition, OutError);
}

// ===== Transition Condition Nodes =====

UEdGraphNode* FAnimGraphEditor::CreateTransitionConditionNode(
	UEdGraph* TransitionGraph,
	const FString& NodeType,
	const TSharedPtr<FJsonObject>& Params,
	int32 PosX,
	int32 PosY,
	FString& OutNodeId,
	FString& OutError)
{
	if (!TransitionGraph)
	{
		OutError = TEXT("Invalid transition graph");
		return nullptr;
	}

	FVector2D Position(PosX, PosY);
	UEdGraphNode* Node = nullptr;

	FString NormalizedType = NodeType.ToLower();

	if (NormalizedType == TEXT("timeremaining"))
	{
		Node = CreateTimeRemainingNode(TransitionGraph, Params, Position, OutError);
	}
	else if (NormalizedType == TEXT("comparefloat") || NormalizedType == TEXT("compare_float"))
	{
		// CompareFloat requires a comparison operator from params
		FString ComparisonOp = TEXT("Less"); // Default
		if (Params.IsValid() && Params->HasField(TEXT("comparison")))
		{
			ComparisonOp = Params->GetStringField(TEXT("comparison"));
		}
		Node = CreateComparisonNode(TransitionGraph, ComparisonOp, Params, Position, OutError);
	}
	else if (NormalizedType == TEXT("comparebool") || NormalizedType == TEXT("compare_bool"))
	{
		// CompareBool uses Equal comparison for booleans
		Node = CreateComparisonNode(TransitionGraph, TEXT("Equal"), Params, Position, OutError);
	}
	else if (NormalizedType == TEXT("and"))
	{
		Node = CreateLogicNode(TransitionGraph, TEXT("And"), Position, OutError);
	}
	else if (NormalizedType == TEXT("or"))
	{
		Node = CreateLogicNode(TransitionGraph, TEXT("Or"), Position, OutError);
	}
	else if (NormalizedType == TEXT("not"))
	{
		Node = CreateLogicNode(TransitionGraph, TEXT("Not"), Position, OutError);
	}
	else if (NormalizedType == TEXT("greater"))
	{
		Node = CreateComparisonNode(TransitionGraph, TEXT("Greater"), Params, Position, OutError);
	}
	else if (NormalizedType == TEXT("less"))
	{
		Node = CreateComparisonNode(TransitionGraph, TEXT("Less"), Params, Position, OutError);
	}
	else if (NormalizedType == TEXT("greaterequal") || NormalizedType == TEXT("greater_equal"))
	{
		Node = CreateComparisonNode(TransitionGraph, TEXT("GreaterEqual"), Params, Position, OutError);
	}
	else if (NormalizedType == TEXT("lessequal") || NormalizedType == TEXT("less_equal"))
	{
		Node = CreateComparisonNode(TransitionGraph, TEXT("LessEqual"), Params, Position, OutError);
	}
	else if (NormalizedType == TEXT("equal"))
	{
		Node = CreateComparisonNode(TransitionGraph, TEXT("Equal"), Params, Position, OutError);
	}
	else if (NormalizedType == TEXT("notequal") || NormalizedType == TEXT("not_equal"))
	{
		Node = CreateComparisonNode(TransitionGraph, TEXT("NotEqual"), Params, Position, OutError);
	}
	else
	{
		OutError = FString::Printf(TEXT("Unknown condition node type: %s. Supported: TimeRemaining, CompareFloat, CompareBool, Greater, Less, GreaterEqual, LessEqual, Equal, NotEqual, And, Or, Not"), *NodeType);
		return nullptr;
	}

	if (Node)
	{
		OutNodeId = GenerateAnimNodeId(TEXT("Cond"), NodeType, TransitionGraph);
		SetNodeId(Node, OutNodeId);
		TransitionGraph->Modify();
	}

	return Node;
}

bool FAnimGraphEditor::ConnectTransitionNodes(
	UEdGraph* TransitionGraph,
	const FString& SourceNodeId,
	const FString& SourcePinName,
	const FString& TargetNodeId,
	const FString& TargetPinName,
	FString& OutError)
{
	if (!TransitionGraph)
	{
		OutError = TEXT("Invalid transition graph");
		return false;
	}

	// Find source node
	UEdGraphNode* SourceNode = FindNodeById(TransitionGraph, SourceNodeId);
	if (!SourceNode)
	{
		OutError = FString::Printf(TEXT("Source node not found: %s"), *SourceNodeId);
		return false;
	}

	// Find target node
	UEdGraphNode* TargetNode = FindNodeById(TransitionGraph, TargetNodeId);
	if (!TargetNode)
	{
		OutError = FString::Printf(TEXT("Target node not found: %s"), *TargetNodeId);
		return false;
	}

	// Find source pin with fallback
	UEdGraphPin* SourcePin = FindPinByName(SourceNode, SourcePinName, EGPD_Output);
	if (!SourcePin)
	{
		// Try common alternative names
		SourcePin = FindPinByName(SourceNode, TEXT("ReturnValue"), EGPD_Output);
	}
	if (!SourcePin)
	{
		SourcePin = FindPinByName(SourceNode, TEXT("Result"), EGPD_Output);
	}
	if (!SourcePin)
	{
		// Debug: list available output pins
		FString AvailablePins;
		for (UEdGraphPin* Pin : SourceNode->Pins)
		{
			if (Pin->Direction == EGPD_Output)
			{
				AvailablePins += FString::Printf(TEXT("[%s] "), *Pin->PinName.ToString());
			}
		}
		OutError = FString::Printf(TEXT("Source pin '%s' not found on node %s. Available output pins: %s"),
			*SourcePinName, *SourceNodeId, AvailablePins.IsEmpty() ? TEXT("none") : *AvailablePins);
		return false;
	}

	// Find target pin with fallback
	UEdGraphPin* TargetPin = FindPinByName(TargetNode, TargetPinName, EGPD_Input);
	if (!TargetPin)
	{
		// Debug: list available input pins
		FString AvailablePins;
		for (UEdGraphPin* Pin : TargetNode->Pins)
		{
			if (Pin->Direction == EGPD_Input)
			{
				AvailablePins += FString::Printf(TEXT("[%s] "), *Pin->PinName.ToString());
			}
		}
		OutError = FString::Printf(TEXT("Target pin '%s' not found on node %s. Available input pins: %s"),
			*TargetPinName, *TargetNodeId, AvailablePins.IsEmpty() ? TEXT("none") : *AvailablePins);
		return false;
	}

	// Make connection
	SourcePin->MakeLinkTo(TargetPin);
	TransitionGraph->Modify();

	return true;
}

bool FAnimGraphEditor::ConnectToTransitionResult(
	UEdGraph* TransitionGraph,
	const FString& ConditionNodeId,
	const FString& ConditionPinName,
	FString& OutError)
{
	if (!TransitionGraph)
	{
		OutError = TEXT("Invalid transition graph");
		return false;
	}

	// Find condition node
	UEdGraphNode* ConditionNode = FindNodeById(TransitionGraph, ConditionNodeId);
	if (!ConditionNode)
	{
		OutError = FString::Printf(TEXT("Condition node not found: %s"), *ConditionNodeId);
		return false;
	}

	// Find condition output pin - try common names
	FString PinName = ConditionPinName.IsEmpty() ? TEXT("ReturnValue") : ConditionPinName;
	UEdGraphPin* ConditionPin = FindPinByName(ConditionNode, PinName, EGPD_Output);
	if (!ConditionPin)
	{
		ConditionPin = FindPinByName(ConditionNode, TEXT("Result"), EGPD_Output);
	}
	if (!ConditionPin)
	{
		ConditionPin = FindPinByName(ConditionNode, TEXT("Output"), EGPD_Output);
	}
	// Fallback: find any boolean output pin
	if (!ConditionPin)
	{
		for (UEdGraphPin* Pin : ConditionNode->Pins)
		{
			if (Pin->Direction == EGPD_Output &&
				Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
			{
				ConditionPin = Pin;
				break;
			}
		}
	}
	if (!ConditionPin)
	{
		// Debug: list available output pins
		FString AvailablePins;
		for (UEdGraphPin* Pin : ConditionNode->Pins)
		{
			if (Pin->Direction == EGPD_Output)
			{
				AvailablePins += FString::Printf(TEXT("[%s] "), *Pin->PinName.ToString());
			}
		}
		OutError = FString::Printf(TEXT("Condition output pin '%s' not found. Available output pins: %s"),
			*PinName, AvailablePins.IsEmpty() ? TEXT("none") : *AvailablePins);
		return false;
	}

	// Find result node
	UEdGraphNode* ResultNode = FindResultNode(TransitionGraph);
	if (!ResultNode)
	{
		OutError = TEXT("Transition result node not found");
		return false;
	}

	// Find bCanEnterTransition pin - try multiple common names for UE 5.7
	UEdGraphPin* ResultPin = FindPinByName(ResultNode, TEXT("bCanEnterTransition"), EGPD_Input);
	if (!ResultPin)
	{
		ResultPin = FindPinByName(ResultNode, TEXT("CanEnterTransition"), EGPD_Input);
	}
	if (!ResultPin)
	{
		ResultPin = FindPinByName(ResultNode, TEXT("Result"), EGPD_Input);
	}
	// Fallback: in UE 5.7, find any boolean input pin on the result node
	if (!ResultPin)
	{
		for (UEdGraphPin* Pin : ResultNode->Pins)
		{
			if (Pin->Direction == EGPD_Input &&
				Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
			{
				ResultPin = Pin;
				break;
			}
		}
	}
	// Final fallback: any input pin
	if (!ResultPin)
	{
		for (UEdGraphPin* Pin : ResultNode->Pins)
		{
			if (Pin->Direction == EGPD_Input)
			{
				ResultPin = Pin;
				break;
			}
		}
	}

	if (!ResultPin)
	{
		// Debug: list available input pins
		FString AvailablePins;
		for (UEdGraphPin* Pin : ResultNode->Pins)
		{
			if (Pin->Direction == EGPD_Input)
			{
				AvailablePins += FString::Printf(TEXT("[%s] "), *Pin->PinName.ToString());
			}
		}
		OutError = FString::Printf(TEXT("Cannot find transition result input pin. Available input pins: %s"),
			AvailablePins.IsEmpty() ? TEXT("none") : *AvailablePins);
		return false;
	}

	// Make connection
	ConditionPin->MakeLinkTo(ResultPin);
	TransitionGraph->Modify();

	return true;
}

// ===== Animation Asset Nodes =====

UEdGraphNode* FAnimGraphEditor::CreateAnimSequenceNode(
	UEdGraph* StateGraph,
	UAnimSequence* AnimSequence,
	FVector2D Position,
	FString& OutNodeId,
	FString& OutError)
{
	if (!StateGraph)
	{
		OutError = TEXT("Invalid state graph");
		return nullptr;
	}

	if (!AnimSequence)
	{
		OutError = TEXT("Invalid animation sequence");
		return nullptr;
	}

	// Create sequence player node
	FGraphNodeCreator<UAnimGraphNode_SequencePlayer> NodeCreator(*StateGraph);
	UAnimGraphNode_SequencePlayer* SeqNode = NodeCreator.CreateNode();

	if (!SeqNode)
	{
		OutError = TEXT("Failed to create sequence player node");
		return nullptr;
	}

	SeqNode->NodePosX = static_cast<int32>(Position.X);
	SeqNode->NodePosY = static_cast<int32>(Position.Y);

	// Set the animation sequence
	SeqNode->Node.SetSequence(AnimSequence);

	NodeCreator.Finalize();

	// Generate ID
	OutNodeId = GenerateAnimNodeId(TEXT("Anim"), AnimSequence->GetName(), StateGraph);
	SetNodeId(SeqNode, OutNodeId);

	StateGraph->Modify();

	return SeqNode;
}

UEdGraphNode* FAnimGraphEditor::CreateBlendSpaceNode(
	UEdGraph* StateGraph,
	UBlendSpace* BlendSpace,
	FVector2D Position,
	FString& OutNodeId,
	FString& OutError)
{
	if (!StateGraph)
	{
		OutError = TEXT("Invalid state graph");
		return nullptr;
	}

	if (!BlendSpace)
	{
		OutError = TEXT("Invalid BlendSpace");
		return nullptr;
	}

	// Create BlendSpace player node
	FGraphNodeCreator<UAnimGraphNode_BlendSpacePlayer> NodeCreator(*StateGraph);
	UAnimGraphNode_BlendSpacePlayer* BSNode = NodeCreator.CreateNode();

	if (!BSNode)
	{
		OutError = TEXT("Failed to create BlendSpace player node");
		return nullptr;
	}

	BSNode->NodePosX = static_cast<int32>(Position.X);
	BSNode->NodePosY = static_cast<int32>(Position.Y);

	// Set the BlendSpace
	BSNode->Node.SetBlendSpace(BlendSpace);

	NodeCreator.Finalize();

	// Generate ID
	OutNodeId = GenerateAnimNodeId(TEXT("BlendSpace"), BlendSpace->GetName(), StateGraph);
	SetNodeId(BSNode, OutNodeId);

	StateGraph->Modify();

	return BSNode;
}

UEdGraphNode* FAnimGraphEditor::CreateBlendSpace1DNode(
	UEdGraph* StateGraph,
	UBlendSpace1D* BlendSpace,
	FVector2D Position,
	FString& OutNodeId,
	FString& OutError)
{
	if (!StateGraph)
	{
		OutError = TEXT("Invalid state graph");
		return nullptr;
	}

	if (!BlendSpace)
	{
		OutError = TEXT("Invalid BlendSpace1D");
		return nullptr;
	}

	// BlendSpace1D uses the same player node
	FGraphNodeCreator<UAnimGraphNode_BlendSpacePlayer> NodeCreator(*StateGraph);
	UAnimGraphNode_BlendSpacePlayer* BSNode = NodeCreator.CreateNode();

	if (!BSNode)
	{
		OutError = TEXT("Failed to create BlendSpace1D player node");
		return nullptr;
	}

	BSNode->NodePosX = static_cast<int32>(Position.X);
	BSNode->NodePosY = static_cast<int32>(Position.Y);

	// Set the BlendSpace
	BSNode->Node.SetBlendSpace(BlendSpace);

	NodeCreator.Finalize();

	// Generate ID
	OutNodeId = GenerateAnimNodeId(TEXT("BlendSpace1D"), BlendSpace->GetName(), StateGraph);
	SetNodeId(BSNode, OutNodeId);

	StateGraph->Modify();

	return BSNode;
}

bool FAnimGraphEditor::ConnectToOutputPose(
	UEdGraph* StateGraph,
	const FString& AnimNodeId,
	FString& OutError)
{
	if (!StateGraph)
	{
		OutError = TEXT("Invalid state graph");
		return false;
	}

	// Find anim node
	UEdGraphNode* AnimNode = FindNodeById(StateGraph, AnimNodeId);
	if (!AnimNode)
	{
		OutError = FString::Printf(TEXT("Animation node not found: %s"), *AnimNodeId);
		return false;
	}

	// Find pose output pin on anim node - try multiple common names
	UEdGraphPin* PosePin = FindPinByName(AnimNode, TEXT("Pose"), EGPD_Output);
	if (!PosePin)
	{
		PosePin = FindPinByName(AnimNode, TEXT("Output"), EGPD_Output);
	}
	if (!PosePin)
	{
		PosePin = FindPinByName(AnimNode, TEXT("Output Pose"), EGPD_Output);
	}
	// Fallback: find any output pin that's a pose link type
	if (!PosePin)
	{
		for (UEdGraphPin* Pin : AnimNode->Pins)
		{
			if (Pin->Direction == EGPD_Output &&
				(Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct ||
				 Pin->PinName.ToString().Contains(TEXT("Pose"), ESearchCase::IgnoreCase)))
			{
				PosePin = Pin;
				break;
			}
		}
	}

	if (!PosePin)
	{
		// Debug: list available output pins
		FString AvailablePins;
		for (UEdGraphPin* Pin : AnimNode->Pins)
		{
			if (Pin->Direction == EGPD_Output)
			{
				AvailablePins += FString::Printf(TEXT("[%s] "), *Pin->PinName.ToString());
			}
		}
		OutError = FString::Printf(TEXT("Animation node has no pose output pin. Available output pins: %s"),
			AvailablePins.IsEmpty() ? TEXT("none") : *AvailablePins);
		return false;
	}

	// Find result node
	UEdGraphNode* ResultNode = FindResultNode(StateGraph);
	if (!ResultNode)
	{
		OutError = TEXT("State result node not found");
		return false;
	}

	// Find result input pin - try multiple common names for UE 5.7
	UEdGraphPin* ResultPin = FindPinByName(ResultNode, TEXT("Result"), EGPD_Input);
	if (!ResultPin)
	{
		ResultPin = FindPinByName(ResultNode, TEXT("Pose"), EGPD_Input);
	}
	if (!ResultPin)
	{
		ResultPin = FindPinByName(ResultNode, TEXT("Output Pose"), EGPD_Input);
	}
	if (!ResultPin)
	{
		ResultPin = FindPinByName(ResultNode, TEXT("InPose"), EGPD_Input);
	}
	// Fallback: in UE 5.7, the StateResult node may have an empty-named pose input pin
	// or it may be the first/only input pin - try finding any input pose link
	if (!ResultPin)
	{
		for (UEdGraphPin* Pin : ResultNode->Pins)
		{
			if (Pin->Direction == EGPD_Input)
			{
				// Accept any input pin on the result node
				ResultPin = Pin;
				break;
			}
		}
	}

	if (!ResultPin)
	{
		// Debug: list available input pins
		FString AvailablePins;
		for (UEdGraphPin* Pin : ResultNode->Pins)
		{
			if (Pin->Direction == EGPD_Input)
			{
				AvailablePins += FString::Printf(TEXT("[%s] "), *Pin->PinName.ToString());
			}
		}
		OutError = FString::Printf(TEXT("Cannot find state result input pin. Available input pins: %s"),
			AvailablePins.IsEmpty() ? TEXT("none") : *AvailablePins);
		return false;
	}

	// Make connection
	PosePin->MakeLinkTo(ResultPin);
	StateGraph->Modify();

	return true;
}

bool FAnimGraphEditor::ClearStateGraph(UEdGraph* StateGraph, FString& OutError)
{
	if (!StateGraph)
	{
		OutError = TEXT("Invalid state graph");
		return false;
	}

	// Collect nodes to remove (exclude result node)
	TArray<UEdGraphNode*> NodesToRemove;
	for (UEdGraphNode* Node : StateGraph->Nodes)
	{
		// Keep result nodes
		if (Node->IsA<UAnimGraphNode_StateResult>() ||
			Node->IsA<UAnimGraphNode_TransitionResult>())
		{
			continue;
		}

		NodesToRemove.Add(Node);
	}

	// Remove nodes
	for (UEdGraphNode* Node : NodesToRemove)
	{
		Node->BreakAllNodeLinks();
		StateGraph->RemoveNode(Node);
	}

	StateGraph->Modify();

	return true;
}

// ===== Node Finding =====

UEdGraphNode* FAnimGraphEditor::FindNodeById(UEdGraph* Graph, const FString& NodeId)
{
	if (!Graph) return nullptr;

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (GetNodeId(Node) == NodeId)
		{
			return Node;
		}
	}

	return nullptr;
}

UEdGraphNode* FAnimGraphEditor::FindResultNode(UEdGraph* Graph)
{
	if (!Graph) return nullptr;

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		// Check for all types of result/root nodes
		if (Node->IsA<UAnimGraphNode_StateResult>() ||
			Node->IsA<UAnimGraphNode_TransitionResult>() ||
			Node->IsA<UAnimGraphNode_Root>())
		{
			return Node;
		}
	}

	return nullptr;
}

UEdGraphPin* FAnimGraphEditor::FindPinByName(
	UEdGraphNode* Node,
	const FString& PinName,
	EEdGraphPinDirection Direction)
{
	if (!Node) return nullptr;

	for (UEdGraphPin* Pin : Node->Pins)
	{
		bool bDirectionMatch = (Direction == EGPD_MAX) || (Pin->Direction == Direction);
		if (bDirectionMatch && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
		{
			return Pin;
		}
	}

	return nullptr;
}

// ===== Node ID System =====

FString FAnimGraphEditor::GenerateAnimNodeId(
	const FString& NodeType,
	const FString& Context,
	UEdGraph* Graph)
{
	int32 Counter = FPlatformAtomics::InterlockedIncrement(&NodeIdCounter);
	FString SafeContext = Context.Replace(TEXT(" "), TEXT("_"));
	FString NodeId = FString::Printf(TEXT("%s_%s_%d"), *NodeType, *SafeContext, Counter);

	// Verify uniqueness
	if (Graph)
	{
		bool bUnique = true;
		do
		{
			bUnique = true;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (GetNodeId(Node) == NodeId)
				{
					Counter = FPlatformAtomics::InterlockedIncrement(&NodeIdCounter);
					NodeId = FString::Printf(TEXT("%s_%s_%d"), *NodeType, *SafeContext, Counter);
					bUnique = false;
					break;
				}
			}
		} while (!bUnique);
	}

	return NodeId;
}

void FAnimGraphEditor::SetNodeId(UEdGraphNode* Node, const FString& NodeId)
{
	if (Node)
	{
		Node->NodeComment = NodeIdPrefix + NodeId;
	}
}

FString FAnimGraphEditor::GetNodeId(UEdGraphNode* Node)
{
	if (Node && Node->NodeComment.StartsWith(NodeIdPrefix))
	{
		return Node->NodeComment.Mid(NodeIdPrefix.Len());
	}
	return FString();
}

TSharedPtr<FJsonObject> FAnimGraphEditor::SerializeAnimNodeInfo(UEdGraphNode* Node)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

	if (!Node) return Json;

	Json->SetStringField(TEXT("node_id"), GetNodeId(Node));
	Json->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
	Json->SetNumberField(TEXT("pos_x"), Node->NodePosX);
	Json->SetNumberField(TEXT("pos_y"), Node->NodePosY);

	// Add pin info
	TArray<TSharedPtr<FJsonValue>> PinsArray;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		TSharedPtr<FJsonObject> PinJson = MakeShared<FJsonObject>();
		PinJson->SetStringField(TEXT("name"), Pin->PinName.ToString());
		PinJson->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));
		PinJson->SetBoolField(TEXT("connected"), Pin->LinkedTo.Num() > 0);
		PinsArray.Add(MakeShared<FJsonValueObject>(PinJson));
	}
	Json->SetArrayField(TEXT("pins"), PinsArray);

	return Json;
}

// ===== Private Helpers =====

UEdGraphNode* FAnimGraphEditor::CreateTimeRemainingNode(UEdGraph* Graph,
	const TSharedPtr<FJsonObject>& Params, FVector2D Position, FString& OutError)
{
	if (!Graph)
	{
		OutError = TEXT("Invalid graph");
		return nullptr;
	}

	// Create the TimeRemaining getter node using UK2Node_TransitionRuleGetter
	FGraphNodeCreator<UK2Node_TransitionRuleGetter> NodeCreator(*Graph);
	UK2Node_TransitionRuleGetter* GetterNode = NodeCreator.CreateNode();

	if (!GetterNode)
	{
		OutError = TEXT("Failed to create TimeRemaining getter node");
		return nullptr;
	}

	GetterNode->NodePosX = static_cast<int32>(Position.X);
	GetterNode->NodePosY = static_cast<int32>(Position.Y);

	// Set to get time remaining from current animation asset
	GetterNode->GetterType = ETransitionGetter::AnimationAsset_GetTimeFromEnd;

	// Try to find an associated animation player node in the source state
	// This is needed for the getter to know which animation to query
	UAnimationTransitionGraph* TransitionGraph = Cast<UAnimationTransitionGraph>(Graph);
	if (TransitionGraph)
	{
		// Get the transition node that owns this graph via GetOuter
		UAnimStateTransitionNode* TransitionNode = Cast<UAnimStateTransitionNode>(TransitionGraph->GetOuter());
		if (TransitionNode)
		{
			UAnimStateNodeBase* PreviousState = TransitionNode->GetPreviousState();
			UEdGraph* StateGraph = PreviousState ? PreviousState->GetBoundGraph() : nullptr;
			if (StateGraph)
			{
				// Find animation player node in the source state
				for (UEdGraphNode* Node : StateGraph->Nodes)
				{
					if (UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node))
					{
						// Check if this is an animation asset player (sequence player, blend space, etc.)
						if (AnimNode->IsA<UAnimGraphNode_SequencePlayer>() ||
							AnimNode->IsA<UAnimGraphNode_BlendSpacePlayer>())
						{
							GetterNode->AssociatedAnimAssetPlayerNode = AnimNode;
							break;
						}
					}
				}
			}
		}
	}

	NodeCreator.Finalize();

	// Allocate default pins
	GetterNode->AllocateDefaultPins();

	return GetterNode;
}

UEdGraphNode* FAnimGraphEditor::CreateComparisonNode(UEdGraph* Graph, const FString& ComparisonType,
	const TSharedPtr<FJsonObject>& Params, FVector2D Position, FString& OutError)
{
	if (!Graph)
	{
		OutError = TEXT("Invalid graph");
		return nullptr;
	}

	// Map comparison type to KismetMathLibrary function name
	FName FunctionName;
	if (ComparisonType.Equals(TEXT("Greater"), ESearchCase::IgnoreCase))
	{
		FunctionName = GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Greater_DoubleDouble);
	}
	else if (ComparisonType.Equals(TEXT("Less"), ESearchCase::IgnoreCase))
	{
		FunctionName = GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Less_DoubleDouble);
	}
	else if (ComparisonType.Equals(TEXT("GreaterEqual"), ESearchCase::IgnoreCase))
	{
		FunctionName = GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, GreaterEqual_DoubleDouble);
	}
	else if (ComparisonType.Equals(TEXT("LessEqual"), ESearchCase::IgnoreCase))
	{
		FunctionName = GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, LessEqual_DoubleDouble);
	}
	else if (ComparisonType.Equals(TEXT("Equal"), ESearchCase::IgnoreCase))
	{
		FunctionName = GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, EqualEqual_DoubleDouble);
	}
	else if (ComparisonType.Equals(TEXT("NotEqual"), ESearchCase::IgnoreCase))
	{
		FunctionName = GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, NotEqual_DoubleDouble);
	}
	else
	{
		OutError = FString::Printf(TEXT("Unknown comparison type: %s. Supported: Greater, Less, GreaterEqual, LessEqual, Equal, NotEqual"), *ComparisonType);
		return nullptr;
	}

	// Find the function in KismetMathLibrary
	UFunction* Function = UKismetMathLibrary::StaticClass()->FindFunctionByName(FunctionName);
	if (!Function)
	{
		OutError = FString::Printf(TEXT("Failed to find comparison function: %s"), *FunctionName.ToString());
		return nullptr;
	}

	// Create the call function node
	FGraphNodeCreator<UK2Node_CallFunction> NodeCreator(*Graph);
	UK2Node_CallFunction* CallNode = NodeCreator.CreateNode();

	if (!CallNode)
	{
		OutError = TEXT("Failed to create comparison call function node");
		return nullptr;
	}

	CallNode->NodePosX = static_cast<int32>(Position.X);
	CallNode->NodePosY = static_cast<int32>(Position.Y);

	// Set the function reference - this is the critical step that was missing before
	CallNode->SetFromFunction(Function);

	NodeCreator.Finalize();

	// Allocate pins based on the function signature
	CallNode->AllocateDefaultPins();

	return CallNode;
}

UEdGraphNode* FAnimGraphEditor::CreateLogicNode(UEdGraph* Graph, const FString& LogicType,
	FVector2D Position, FString& OutError)
{
	if (!Graph)
	{
		OutError = TEXT("Invalid graph");
		return nullptr;
	}

	// Map logic type to KismetMathLibrary function name
	FName FunctionName;
	if (LogicType.Equals(TEXT("And"), ESearchCase::IgnoreCase))
	{
		FunctionName = GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, BooleanAND);
	}
	else if (LogicType.Equals(TEXT("Or"), ESearchCase::IgnoreCase))
	{
		FunctionName = GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, BooleanOR);
	}
	else if (LogicType.Equals(TEXT("Not"), ESearchCase::IgnoreCase))
	{
		FunctionName = GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Not_PreBool);
	}
	else
	{
		OutError = FString::Printf(TEXT("Unknown logic type: %s. Supported: And, Or, Not"), *LogicType);
		return nullptr;
	}

	// Find the function in KismetMathLibrary
	UFunction* Function = UKismetMathLibrary::StaticClass()->FindFunctionByName(FunctionName);
	if (!Function)
	{
		OutError = FString::Printf(TEXT("Failed to find logic function: %s"), *FunctionName.ToString());
		return nullptr;
	}

	// Create the call function node
	FGraphNodeCreator<UK2Node_CallFunction> NodeCreator(*Graph);
	UK2Node_CallFunction* CallNode = NodeCreator.CreateNode();

	if (!CallNode)
	{
		OutError = TEXT("Failed to create logic call function node");
		return nullptr;
	}

	CallNode->NodePosX = static_cast<int32>(Position.X);
	CallNode->NodePosY = static_cast<int32>(Position.Y);

	// Set the function reference - this is the critical step that was missing before
	CallNode->SetFromFunction(Function);

	NodeCreator.Finalize();

	// Allocate pins based on the function signature
	CallNode->AllocateDefaultPins();

	return CallNode;
}

UEdGraphNode* FAnimGraphEditor::CreateVariableGetNode(UEdGraph* Graph, UAnimBlueprint* AnimBP,
	const FString& VariableName, FVector2D Position, FString& OutError)
{
	if (!Graph || !AnimBP)
	{
		OutError = TEXT("Invalid graph or AnimBlueprint");
		return nullptr;
	}

	// Find the variable property
	FProperty* Property = FindFProperty<FProperty>(AnimBP->GeneratedClass, *VariableName);
	if (!Property)
	{
		Property = FindFProperty<FProperty>(AnimBP->SkeletonGeneratedClass, *VariableName);
	}

	if (!Property)
	{
		OutError = FString::Printf(TEXT("Variable '%s' not found in AnimBlueprint"), *VariableName);
		return nullptr;
	}

	// Create variable get node
	FGraphNodeCreator<UK2Node_VariableGet> NodeCreator(*Graph);
	UK2Node_VariableGet* VarNode = NodeCreator.CreateNode();

	if (!VarNode)
	{
		OutError = TEXT("Failed to create variable get node");
		return nullptr;
	}

	VarNode->NodePosX = static_cast<int32>(Position.X);
	VarNode->NodePosY = static_cast<int32>(Position.Y);
	VarNode->VariableReference.SetSelfMember(FName(*VariableName));

	NodeCreator.Finalize();

	return VarNode;
}

// ===== AnimGraph Root Connection =====

UAnimGraphNode_Root* FAnimGraphEditor::FindAnimGraphRoot(UAnimBlueprint* AnimBP, FString& OutError)
{
	if (!AnimBP)
	{
		OutError = TEXT("Invalid Animation Blueprint");
		return nullptr;
	}

	// Find the main AnimGraph
	UEdGraph* AnimGraph = FindAnimGraph(AnimBP, OutError);
	if (!AnimGraph)
	{
		return nullptr;
	}

	// Find the root node in the AnimGraph
	for (UEdGraphNode* Node : AnimGraph->Nodes)
	{
		if (UAnimGraphNode_Root* RootNode = Cast<UAnimGraphNode_Root>(Node))
		{
			return RootNode;
		}
	}

	OutError = TEXT("AnimGraph root node (Output Pose) not found");
	return nullptr;
}

bool FAnimGraphEditor::ConnectStateMachineToAnimGraphRoot(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	FString& OutError)
{
	if (!AnimBP)
	{
		OutError = TEXT("Invalid Animation Blueprint");
		return false;
	}

	// Find the AnimGraph
	UEdGraph* AnimGraph = FindAnimGraph(AnimBP, OutError);
	if (!AnimGraph)
	{
		return false;
	}

	// Find the State Machine node in the AnimGraph
	UAnimGraphNode_StateMachine* StateMachineNode = FAnimStateMachineEditor::FindStateMachine(AnimBP, StateMachineName, OutError);
	if (!StateMachineNode)
	{
		return false;
	}

	// Find the AnimGraph root node
	UAnimGraphNode_Root* RootNode = FindAnimGraphRoot(AnimBP, OutError);
	if (!RootNode)
	{
		return false;
	}

	// Find the State Machine's output pose pin - try multiple common names
	UEdGraphPin* SMOutputPin = FindPinByName(StateMachineNode, TEXT("Pose"), EGPD_Output);
	if (!SMOutputPin)
	{
		SMOutputPin = FindPinByName(StateMachineNode, TEXT("Output"), EGPD_Output);
	}
	if (!SMOutputPin)
	{
		SMOutputPin = FindPinByName(StateMachineNode, TEXT("Output Pose"), EGPD_Output);
	}
	// Fallback: find any output pin
	if (!SMOutputPin)
	{
		for (UEdGraphPin* Pin : StateMachineNode->Pins)
		{
			if (Pin->Direction == EGPD_Output)
			{
				SMOutputPin = Pin;
				break;
			}
		}
	}

	if (!SMOutputPin)
	{
		// Debug: list available pins
		FString AvailablePins;
		for (UEdGraphPin* Pin : StateMachineNode->Pins)
		{
			AvailablePins += FString::Printf(TEXT("[%s:%s] "),
				*Pin->PinName.ToString(),
				Pin->Direction == EGPD_Input ? TEXT("In") : TEXT("Out"));
		}
		OutError = FString::Printf(TEXT("State Machine '%s' has no output pose pin. Available pins: %s"),
			*StateMachineName, AvailablePins.IsEmpty() ? TEXT("none") : *AvailablePins);
		return false;
	}

	// Find the AnimGraph root's input pin - try multiple common names
	UEdGraphPin* RootInputPin = FindPinByName(RootNode, TEXT("Result"), EGPD_Input);
	if (!RootInputPin)
	{
		RootInputPin = FindPinByName(RootNode, TEXT("Pose"), EGPD_Input);
	}
	if (!RootInputPin)
	{
		RootInputPin = FindPinByName(RootNode, TEXT("InPose"), EGPD_Input);
	}
	// Fallback: find any input pin
	if (!RootInputPin)
	{
		for (UEdGraphPin* Pin : RootNode->Pins)
		{
			if (Pin->Direction == EGPD_Input)
			{
				RootInputPin = Pin;
				break;
			}
		}
	}

	if (!RootInputPin)
	{
		// Debug: list available pins
		FString AvailablePins;
		for (UEdGraphPin* Pin : RootNode->Pins)
		{
			AvailablePins += FString::Printf(TEXT("[%s:%s] "),
				*Pin->PinName.ToString(),
				Pin->Direction == EGPD_Input ? TEXT("In") : TEXT("Out"));
		}
		OutError = FString::Printf(TEXT("AnimGraph root node has no input pose pin. Available pins: %s"),
			AvailablePins.IsEmpty() ? TEXT("none") : *AvailablePins);
		return false;
	}

	// Break any existing connections on the root input
	RootInputPin->BreakAllPinLinks();

	// Make the connection
	SMOutputPin->MakeLinkTo(RootInputPin);
	AnimGraph->Modify();

	return true;
}

// ===== Transition Graph Node Operations =====

TSharedPtr<FJsonObject> FAnimGraphEditor::SerializeDetailedPinInfo(UEdGraphPin* Pin)
{
	TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();

	if (!Pin) return PinObj;

	PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
	PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));

	// Detailed type info
	FString TypeStr;
	FString SubCategoryStr;

	if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
	{
		TypeStr = TEXT("bool");
	}
	else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int)
	{
		TypeStr = TEXT("int32");
	}
	else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int64)
	{
		TypeStr = TEXT("int64");
	}
	else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Real)
	{
		TypeStr = (Pin->PinType.PinSubCategory == UEdGraphSchema_K2::PC_Double) ? TEXT("double") : TEXT("float");
	}
	else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_String)
	{
		TypeStr = TEXT("FString");
	}
	else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Name)
	{
		TypeStr = TEXT("FName");
	}
	else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Text)
	{
		TypeStr = TEXT("FText");
	}
	else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
	{
		TypeStr = TEXT("exec");
	}
	else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
	{
		if (UScriptStruct* Struct = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get()))
		{
			TypeStr = TEXT("struct");
			SubCategoryStr = Struct->GetName();
		}
	}
	else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object)
	{
		if (UClass* Class = Cast<UClass>(Pin->PinType.PinSubCategoryObject.Get()))
		{
			TypeStr = TEXT("object");
			SubCategoryStr = Class->GetName();
		}
	}
	else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class)
	{
		TypeStr = TEXT("class");
	}
	else
	{
		TypeStr = Pin->PinType.PinCategory.ToString();
	}

	PinObj->SetStringField(TEXT("type"), TypeStr);
	if (!SubCategoryStr.IsEmpty())
	{
		PinObj->SetStringField(TEXT("sub_type"), SubCategoryStr);
	}

	// Default value
	if (!Pin->DefaultValue.IsEmpty())
	{
		PinObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
	}
	if (!Pin->AutogeneratedDefaultValue.IsEmpty())
	{
		PinObj->SetStringField(TEXT("auto_default_value"), Pin->AutogeneratedDefaultValue);
	}

	// Connection info
	PinObj->SetBoolField(TEXT("is_connected"), Pin->LinkedTo.Num() > 0);
	PinObj->SetNumberField(TEXT("connection_count"), Pin->LinkedTo.Num());

	// Connected to
	if (Pin->LinkedTo.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ConnectedTo;
		for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			if (LinkedPin && LinkedPin->GetOwningNode())
			{
				TSharedPtr<FJsonObject> LinkObj = MakeShared<FJsonObject>();
				LinkObj->SetStringField(TEXT("node_id"), GetNodeId(LinkedPin->GetOwningNode()));
				LinkObj->SetStringField(TEXT("pin_name"), LinkedPin->PinName.ToString());
				ConnectedTo.Add(MakeShared<FJsonValueObject>(LinkObj));
			}
		}
		PinObj->SetArrayField(TEXT("connected_to"), ConnectedTo);
	}

	return PinObj;
}

TSharedPtr<FJsonObject> FAnimGraphEditor::GetTransitionGraphNodes(
	UEdGraph* TransitionGraph,
	FString& OutError)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	if (!TransitionGraph)
	{
		OutError = TEXT("Invalid transition graph");
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), OutError);
		return Result;
	}

	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("graph_name"), TransitionGraph->GetName());

	TArray<TSharedPtr<FJsonValue>> NodesArray;

	for (UEdGraphNode* Node : TransitionGraph->Nodes)
	{
		if (!Node) continue;

		TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();

		// Basic node info
		FString NodeId = GetNodeId(Node);
		NodeObj->SetStringField(TEXT("node_id"), NodeId.IsEmpty() ? TEXT("(unnamed)") : NodeId);
		NodeObj->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
		NodeObj->SetStringField(TEXT("node_title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
		NodeObj->SetNumberField(TEXT("pos_x"), Node->NodePosX);
		NodeObj->SetNumberField(TEXT("pos_y"), Node->NodePosY);

		// Check if this is the result node
		bool bIsResultNode = Node->IsA<UAnimGraphNode_TransitionResult>();
		NodeObj->SetBoolField(TEXT("is_result_node"), bIsResultNode);

		// Detailed pins
		TArray<TSharedPtr<FJsonValue>> InputPins;
		TArray<TSharedPtr<FJsonValue>> OutputPins;

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) continue;

			TSharedPtr<FJsonObject> PinObj = SerializeDetailedPinInfo(Pin);

			if (Pin->Direction == EGPD_Input)
			{
				InputPins.Add(MakeShared<FJsonValueObject>(PinObj));
			}
			else
			{
				OutputPins.Add(MakeShared<FJsonValueObject>(PinObj));
			}
		}

		NodeObj->SetArrayField(TEXT("input_pins"), InputPins);
		NodeObj->SetArrayField(TEXT("output_pins"), OutputPins);

		NodesArray.Add(MakeShared<FJsonValueObject>(NodeObj));
	}

	Result->SetArrayField(TEXT("nodes"), NodesArray);
	Result->SetNumberField(TEXT("node_count"), NodesArray.Num());

	return Result;
}

TSharedPtr<FJsonObject> FAnimGraphEditor::GetAllTransitionNodes(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	FString& OutError)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	if (!AnimBP)
	{
		OutError = TEXT("Invalid Animation Blueprint");
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), OutError);
		return Result;
	}

	// Find state machine
	UAnimGraphNode_StateMachine* StateMachine = FAnimStateMachineEditor::FindStateMachine(AnimBP, StateMachineName, OutError);
	if (!StateMachine)
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), OutError);
		return Result;
	}

	// Get state machine name for the API call (use same name passed in for consistency)
	FString SMName = StateMachine->GetStateMachineName();

	// Get all transitions (AnimBP is already a parameter)
	TArray<UAnimStateTransitionNode*> Transitions = FAnimStateMachineEditor::GetAllTransitions(AnimBP, SMName, OutError);

	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("state_machine"), StateMachineName);

	TArray<TSharedPtr<FJsonValue>> TransitionsArray;

	for (UAnimStateTransitionNode* Transition : Transitions)
	{
		if (!Transition) continue;

		TSharedPtr<FJsonObject> TransitionObj = MakeShared<FJsonObject>();

		// Get source/target state names
		FString FromState, ToState;
		if (Transition->GetPreviousState())
		{
			FromState = Transition->GetPreviousState()->GetStateName();
		}
		if (Transition->GetNextState())
		{
			ToState = Transition->GetNextState()->GetStateName();
		}

		TransitionObj->SetStringField(TEXT("from_state"), FromState);
		TransitionObj->SetStringField(TEXT("to_state"), ToState);
		TransitionObj->SetStringField(TEXT("transition_name"), FString::Printf(TEXT("%s -> %s"), *FromState, *ToState));

		// Get transition graph
		UEdGraph* TransGraph = FAnimStateMachineEditor::GetTransitionGraph(Transition, OutError);
		if (TransGraph)
		{
			TSharedPtr<FJsonObject> NodesInfo = GetTransitionGraphNodes(TransGraph, OutError);
			TransitionObj->SetObjectField(TEXT("graph"), NodesInfo);
		}

		TransitionsArray.Add(MakeShared<FJsonValueObject>(TransitionObj));
	}

	Result->SetArrayField(TEXT("transitions"), TransitionsArray);
	Result->SetNumberField(TEXT("transition_count"), TransitionsArray.Num());

	return Result;
}

bool FAnimGraphEditor::ValidatePinValueType(
	UEdGraphPin* Pin,
	const FString& Value,
	FString& OutError)
{
	if (!Pin)
	{
		OutError = TEXT("Invalid pin");
		return false;
	}

	// Validate based on pin type
	if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
	{
		FString LowerValue = Value.ToLower();
		if (LowerValue != TEXT("true") && LowerValue != TEXT("false") &&
			LowerValue != TEXT("1") && LowerValue != TEXT("0"))
		{
			OutError = FString::Printf(TEXT("Pin '%s' expects bool value (true/false), got: %s"),
				*Pin->PinName.ToString(), *Value);
			return false;
		}
	}
	else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int ||
			 Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int64)
	{
		if (!Value.IsNumeric())
		{
			OutError = FString::Printf(TEXT("Pin '%s' expects integer value, got: %s"),
				*Pin->PinName.ToString(), *Value);
			return false;
		}
		// Check for decimal point (integers shouldn't have them)
		if (Value.Contains(TEXT(".")))
		{
			OutError = FString::Printf(TEXT("Pin '%s' expects integer value (no decimals), got: %s"),
				*Pin->PinName.ToString(), *Value);
			return false;
		}
	}
	else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Real)
	{
		if (!FCString::Atod(*Value) && Value != TEXT("0") && Value != TEXT("0.0"))
		{
			// Additional validation for numeric strings
			bool bIsValid = true;
			bool bFoundDot = false;
			for (TCHAR C : Value)
			{
				if (C == '.' && !bFoundDot)
				{
					bFoundDot = true;
				}
				else if (C == '-' && Value[0] == '-')
				{
					continue;
				}
				else if (!FChar::IsDigit(C))
				{
					bIsValid = false;
					break;
				}
			}
			if (!bIsValid)
			{
				OutError = FString::Printf(TEXT("Pin '%s' expects float/double value, got: %s"),
					*Pin->PinName.ToString(), *Value);
				return false;
			}
		}
	}
	else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
	{
		OutError = FString::Printf(TEXT("Pin '%s' is an exec pin, cannot set default value"),
			*Pin->PinName.ToString());
		return false;
	}
	// For strings, names, text - any value is valid
	// For structs and objects - we allow JSON-style values but don't deeply validate

	return true;
}

bool FAnimGraphEditor::SetPinDefaultValueWithValidation(
	UEdGraph* Graph,
	const FString& NodeId,
	const FString& PinName,
	const FString& Value,
	FString& OutError)
{
	if (!Graph)
	{
		OutError = TEXT("Invalid graph");
		return false;
	}

	// Find node
	UEdGraphNode* Node = FindNodeById(Graph, NodeId);
	if (!Node)
	{
		OutError = FString::Printf(TEXT("Node not found: %s"), *NodeId);
		return false;
	}

	// Find pin
	UEdGraphPin* Pin = FindPinByName(Node, PinName, EGPD_Input);
	if (!Pin)
	{
		// List available input pins
		FString AvailablePins;
		for (UEdGraphPin* P : Node->Pins)
		{
			if (P->Direction == EGPD_Input)
			{
				AvailablePins += FString::Printf(TEXT("[%s] "), *P->PinName.ToString());
			}
		}
		OutError = FString::Printf(TEXT("Input pin '%s' not found on node %s. Available: %s"),
			*PinName, *NodeId, AvailablePins.IsEmpty() ? TEXT("none") : *AvailablePins);
		return false;
	}

	// Validate value type
	if (!ValidatePinValueType(Pin, Value, OutError))
	{
		return false;
	}

	// Set the default value
	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (Schema)
	{
		Schema->TrySetDefaultValue(*Pin, Value);
	}
	else
	{
		Pin->DefaultValue = Value;
	}

	Graph->Modify();

	return true;
}

TSharedPtr<FJsonObject> FAnimGraphEditor::CreateComparisonChain(
	UAnimBlueprint* AnimBP,
	UEdGraph* TransitionGraph,
	const FString& VariableName,
	const FString& ComparisonType,
	const FString& CompareValue,
	FVector2D Position,
	FString& OutError)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	if (!AnimBP || !TransitionGraph)
	{
		OutError = TEXT("Invalid AnimBlueprint or transition graph");
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), OutError);
		return Result;
	}

	// Step 1: Create GetVariable node
	FString GetVarNodeId;
	UEdGraphNode* GetVarNode = CreateVariableGetNode(TransitionGraph, AnimBP, VariableName, Position, OutError);
	if (!GetVarNode)
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), OutError);
		return Result;
	}
	GetVarNodeId = GenerateAnimNodeId(TEXT("GetVar"), VariableName, TransitionGraph);
	SetNodeId(GetVarNode, GetVarNodeId);
	GetVarNode->AllocateDefaultPins();

	// Step 2: Create Comparison node
	FVector2D CompPos(Position.X + 200, Position.Y);
	TSharedPtr<FJsonObject> CompParams = MakeShared<FJsonObject>();
	UEdGraphNode* CompNode = CreateComparisonNode(TransitionGraph, ComparisonType, CompParams, CompPos, OutError);
	if (!CompNode)
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), OutError);
		return Result;
	}
	FString CompNodeId = GenerateAnimNodeId(TEXT("Comp"), ComparisonType, TransitionGraph);
	SetNodeId(CompNode, CompNodeId);

	// Step 3: Connect GetVariable output to Comparison input A
	UEdGraphPin* VarOutputPin = nullptr;
	for (UEdGraphPin* Pin : GetVarNode->Pins)
	{
		if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
		{
			VarOutputPin = Pin;
			break;
		}
	}

	UEdGraphPin* CompInputA = FindPinByName(CompNode, TEXT("A"), EGPD_Input);
	if (!CompInputA)
	{
		// Try finding first non-exec input
		for (UEdGraphPin* Pin : CompNode->Pins)
		{
			if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
			{
				CompInputA = Pin;
				break;
			}
		}
	}

	if (VarOutputPin && CompInputA)
	{
		VarOutputPin->MakeLinkTo(CompInputA);
	}

	// Step 4: Set comparison value on pin B
	UEdGraphPin* CompInputB = FindPinByName(CompNode, TEXT("B"), EGPD_Input);
	if (CompInputB && !CompareValue.IsEmpty())
	{
		const UEdGraphSchema* Schema = TransitionGraph->GetSchema();
		if (Schema)
		{
			Schema->TrySetDefaultValue(*CompInputB, CompareValue);
		}
		else
		{
			CompInputB->DefaultValue = CompareValue;
		}
	}

	// Step 5: Find the result node and check if there's existing logic
	UEdGraphNode* ResultNode = FindResultNode(TransitionGraph);
	UEdGraphPin* ResultInputPin = nullptr;
	UEdGraphPin* ExistingConnection = nullptr;

	if (ResultNode)
	{
		// Find the boolean input pin on the result node
		ResultInputPin = FindPinByName(ResultNode, TEXT("bCanEnterTransition"), EGPD_Input);
		if (!ResultInputPin)
		{
			ResultInputPin = FindPinByName(ResultNode, TEXT("CanEnterTransition"), EGPD_Input);
		}
		if (!ResultInputPin)
		{
			for (UEdGraphPin* Pin : ResultNode->Pins)
			{
				if (Pin->Direction == EGPD_Input &&
					Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
				{
					ResultInputPin = Pin;
					break;
				}
			}
		}

		// Check if there's existing logic connected to result
		if (ResultInputPin && ResultInputPin->LinkedTo.Num() > 0)
		{
			ExistingConnection = ResultInputPin->LinkedTo[0];
		}
	}

	// Get comparison output pin
	UEdGraphPin* CompOutputPin = FindPinByName(CompNode, TEXT("ReturnValue"), EGPD_Output);
	if (!CompOutputPin)
	{
		for (UEdGraphPin* Pin : CompNode->Pins)
		{
			if (Pin->Direction == EGPD_Output &&
				Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
			{
				CompOutputPin = Pin;
				break;
			}
		}
	}

	// Step 6: Auto-chain with AND if existing logic present
	FString AndNodeId;
	if (ExistingConnection && CompOutputPin && ResultInputPin)
	{
		// Create AND node
		FVector2D AndPos(Position.X + 400, Position.Y);
		UEdGraphNode* AndNode = CreateLogicNode(TransitionGraph, TEXT("And"), AndPos, OutError);
		if (AndNode)
		{
			AndNodeId = GenerateAnimNodeId(TEXT("And"), TEXT("Chain"), TransitionGraph);
			SetNodeId(AndNode, AndNodeId);

			// Disconnect existing connection from result
			ResultInputPin->BreakAllPinLinks();

			// Connect existing logic to AND input A
			UEdGraphPin* AndInputA = FindPinByName(AndNode, TEXT("A"), EGPD_Input);
			if (AndInputA)
			{
				ExistingConnection->MakeLinkTo(AndInputA);
			}

			// Connect new comparison to AND input B
			UEdGraphPin* AndInputB = FindPinByName(AndNode, TEXT("B"), EGPD_Input);
			if (AndInputB)
			{
				CompOutputPin->MakeLinkTo(AndInputB);
			}

			// Connect AND output to result
			UEdGraphPin* AndOutput = FindPinByName(AndNode, TEXT("ReturnValue"), EGPD_Output);
			if (!AndOutput)
			{
				for (UEdGraphPin* Pin : AndNode->Pins)
				{
					if (Pin->Direction == EGPD_Output &&
						Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
					{
						AndOutput = Pin;
						break;
					}
				}
			}
			if (AndOutput && ResultInputPin)
			{
				AndOutput->MakeLinkTo(ResultInputPin);
			}
		}
	}
	else if (CompOutputPin && ResultInputPin)
	{
		// No existing logic, connect directly to result
		ResultInputPin->BreakAllPinLinks();
		CompOutputPin->MakeLinkTo(ResultInputPin);
	}

	TransitionGraph->Modify();

	// Build result
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("variable_node_id"), GetVarNodeId);
	Result->SetStringField(TEXT("comparison_node_id"), CompNodeId);
	if (!AndNodeId.IsEmpty())
	{
		Result->SetStringField(TEXT("and_node_id"), AndNodeId);
		Result->SetBoolField(TEXT("chained_with_existing"), true);
	}
	else
	{
		Result->SetBoolField(TEXT("chained_with_existing"), false);
	}
	Result->SetStringField(TEXT("variable"), VariableName);
	Result->SetStringField(TEXT("comparison"), ComparisonType);
	Result->SetStringField(TEXT("value"), CompareValue);

	return Result;
}

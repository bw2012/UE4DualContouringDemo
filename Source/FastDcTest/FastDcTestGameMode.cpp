// Copyright 1998-2018 Epic Games, Inc. All Rights Reserved.

#include "FastDcTestGameMode.h"
#include "FastDcTestPlayerController.h"
#include "FastDcTestCharacter.h"
#include "UObject/ConstructorHelpers.h"

AFastDcTestGameMode::AFastDcTestGameMode()
{
	// use our custom PlayerController class
	PlayerControllerClass = AFastDcTestPlayerController::StaticClass();

	// set default pawn class to our Blueprinted character
	static ConstructorHelpers::FClassFinder<APawn> PlayerPawnBPClass(TEXT("/Game/TopDownCPP/Blueprints/TopDownCharacter"));
	if (PlayerPawnBPClass.Class != NULL)
	{
		DefaultPawnClass = PlayerPawnBPClass.Class;
	}
}
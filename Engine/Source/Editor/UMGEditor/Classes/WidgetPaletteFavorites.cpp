// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "WidgetPaletteFavorites.h"
#include "UObject/UnrealType.h"

UWidgetPaletteFavorites::UWidgetPaletteFavorites(FObjectInitializer const& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UWidgetPaletteFavorites::Add(const FString& InWidgetTemplateName)
{
	Favorites.AddUnique(InWidgetTemplateName);

	SaveConfig();

	OnFavoritesUpdated.Broadcast();
}

void UWidgetPaletteFavorites::Remove(const FString& InWidgetTemplateName)
{
	Favorites.Remove(InWidgetTemplateName);

	SaveConfig();

	OnFavoritesUpdated.Broadcast();
}
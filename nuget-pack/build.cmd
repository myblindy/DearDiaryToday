msbuild .. /t:Rebuild /p:Configuration=Release /p:Platform=x64 /p:ContinuousIntegrationBuild=true
del *.nupkg
nuget pack MB.DearDiaryToday.x64.nuspec
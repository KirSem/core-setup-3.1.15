// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using FluentAssertions;
using FluentAssertions.Execution;
using System;
using System.Collections.Generic;
using System.IO;

namespace Microsoft.DotNet.CoreSetup.Test.HostActivation.DependencyResolution
{
    public static class DependencyResolutionCommandResultExtensions
    {
        public const string TRUSTED_PLATFORM_ASSEMBLIES = "TRUSTED_PLATFORM_ASSEMBLIES";
        public const string NATIVE_DLL_SEARCH_DIRECTORIES = "NATIVE_DLL_SEARCH_DIRECTORIES";

        public static AndConstraint<CommandResultAssertions> HaveRuntimePropertyContaining(this CommandResultAssertions assertion, string propertyName, params string[] values)
        {
            string propertyValue = GetMockPropertyValue(assertion, propertyName);

            foreach (string value in values)
            {
                Execute.Assertion.ForCondition(propertyValue != null && propertyValue.Contains(value))
                    .FailWith("The property {0} doesn't contain expected value: {1}\n{2}\n{3}", propertyName, value, propertyValue, assertion.GetDiagnosticsInfo());
            }

            return new AndConstraint<CommandResultAssertions>(assertion);
        }

        public static AndConstraint<CommandResultAssertions> NotHaveRuntimePropertyContaining(this CommandResultAssertions assertion, string propertyName, params string[] values)
        {
            string propertyValue = GetMockPropertyValue(assertion, propertyName);

            foreach (string value in values)
            {
                Execute.Assertion.ForCondition(propertyValue != null && !propertyValue.Contains(value))
                    .FailWith("The property {0} contains unexpected value: {1}\n{2}\n{3}", propertyName, value, propertyValue, assertion.GetDiagnosticsInfo());
            }

            return new AndConstraint<CommandResultAssertions>(assertion);
        }

        public static AndConstraint<CommandResultAssertions> HaveResolvedAssembly(this CommandResultAssertions assertion, string assemblyPath, TestApp app = null)
        {
            return assertion.HaveRuntimePropertyContaining(TRUSTED_PLATFORM_ASSEMBLIES, RelativePathsToAbsoluteAppPaths(assemblyPath, app));
        }

        public static AndConstraint<CommandResultAssertions> NotHaveResolvedAssembly(this CommandResultAssertions assertion, string assemblyPath, TestApp app = null)
        {
            return assertion.NotHaveRuntimePropertyContaining(TRUSTED_PLATFORM_ASSEMBLIES, RelativePathsToAbsoluteAppPaths(assemblyPath, app));
        }

        public static AndConstraint<CommandResultAssertions> HaveResolvedNativeLibraryPath(this CommandResultAssertions assertion, string path, TestApp app = null)
        {
            return assertion.HaveRuntimePropertyContaining(NATIVE_DLL_SEARCH_DIRECTORIES, RelativePathsToAbsoluteAppPaths(path, app));
        }

        public static AndConstraint<CommandResultAssertions> NotHaveResolvedNativeLibraryPath(this CommandResultAssertions assertion, string path, TestApp app = null)
        {
            return assertion.NotHaveRuntimePropertyContaining(NATIVE_DLL_SEARCH_DIRECTORIES, RelativePathsToAbsoluteAppPaths(path, app));
        }

        private static string GetMockPropertyValue(CommandResultAssertions assertion, string propertyName)
        {
            string propertyHeader = $"mock property[{propertyName}] = ";
            string stdout = assertion.Result.StdOut;
            int i = stdout.IndexOf(propertyHeader);
            if (i >= 0)
            {
                i += propertyHeader.Length;
                int end = assertion.Result.StdOut.IndexOf(Environment.NewLine, i);
                if (end >= i)
                {
                    return stdout.Substring(i, end - i);
                }
            }

            return null;
        }

        private static string[] RelativePathsToAbsoluteAppPaths(string relativePaths, TestApp app)
        {
            List<string> paths = new List<string>();
            foreach (string relativePath in relativePaths.Split(';'))
            {
                string path = relativePath.Replace('/', Path.DirectorySeparatorChar);
                if (app != null)
                {
                    path = Path.Combine(app.Location, path);
                }

                paths.Add(path);
            }

            return paths.ToArray();
        }
    }
}

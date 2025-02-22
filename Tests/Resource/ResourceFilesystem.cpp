// Copyright (C) 2009-present, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <Tests/Framework/Framework.h>
#include <AnKi/Resource/ResourceFilesystem.h>

ANKI_TEST(Resource, ResourceFilesystem)
{
	printf("Test requires the Data dir\n");

	ResourceFilesystem fs;
	ANKI_TEST_EXPECT_NO_ERR(fs.init());

	{
		ANKI_TEST_EXPECT_NO_ERR(fs.addNewPath("Tests/Data/Dir/../Dir/", ResourceStringList(), ResourceStringList()));
		ResourceFilePtr file;
		ANKI_TEST_EXPECT_NO_ERR(fs.openFile("subdir0/hello.txt", file));
		ResourceString txt;
		ANKI_TEST_EXPECT_NO_ERR(file->readAllText(txt));
		ANKI_TEST_EXPECT_EQ(txt, "hello\n");
	}

	{
		ANKI_TEST_EXPECT_NO_ERR(fs.addNewPath("./Tests/Data/Dir.AnKiZLibip", ResourceStringList(), ResourceStringList()));
		ResourceFilePtr file;
		ANKI_TEST_EXPECT_NO_ERR(fs.openFile("subdir0/hello.txt", file));
		ResourceString txt;
		ANKI_TEST_EXPECT_NO_ERR(file->readAllText(txt));
		ANKI_TEST_EXPECT_EQ(txt, "hell\n");
	}
}

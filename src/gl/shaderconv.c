#include "shaderconv.h"

#include <stdio.h>
#include "../glx/hardext.h"
#include "debug.h"
#include "fpe_shader.h"
#include "init.h"
#include "preproc.h"
#include "string_utils.h"
#include "shader_hacks.h"

#include "loader.h"
#include "pack/shaderconv.h"
#include "../glsl/glsl_for_es.h"
#include "../glx/hardext.h"

#include <GL/gl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>
#include <math.h>

int NO_OPERATOR_VALUE = 9999;

static const char* switch_template = " switch (%n%[^)] { ";
static const char* case_template = " case %n%[^:] ";
static const char* declaration_template = " const float %s = %s ;";

#define VARIABLE_SIZE 1024
#define MODE_SWITCH 0
#define MODE_CASE 1

char * BackportConstArrays(char *source, int * sourceLength){
    unsigned long startPoint = strstrPos(source, "const");
    if(startPoint == 0){
        return source;
    }
    int constStart, constStop;
    GetNextWord(source, startPoint, &constStart, &constStop); // Catch the "const"

    int typeStart, typeStop;
    GetNextWord(source, constStop, &typeStart, &typeStop); // Catch the type, without []

    int variableNameStart, variableNameStop;
    GetNextWord(source, typeStop, &variableNameStart, &variableNameStop); // Catch the var name
    char * variableName = ExtractString(source, variableNameStart, variableNameStop);

    //Now, verify the data type is actually an array
    char * tokenStart = strstr(source + typeStop, "[");
    if( tokenStart != NULL && (tokenStart - source) < variableNameStart){
        // We've found an array. So we need to get to the starting parenthesis and isolate each member
        int startArray = GetNextTokenPosition(source, variableNameStop, '(', "");
        int endArray = GetClosingTokenPosition(source, startArray);

        // First pass, to count the amount of entries in the array
        int arrayEntryCount = -1;
        int currentPoint = startArray;
        while (currentPoint < endArray){
            ++arrayEntryCount;
            currentPoint = GetClosingTokenPositionTokenOverride(source, currentPoint, ',');
        }
        if(currentPoint == endArray){
            ++arrayEntryCount;
        }

        // Now we know how many entries we have, we can copy data
        int entryStart = startArray + 1;
        int entryEnd;
        for(int j=0; j<arrayEntryCount; ++j){
            // First, isolate the array entry
            entryEnd = GetClosingTokenPositionTokenOverride(source, entryStart, ',');

            // Replace the entry and jump to the end of the replacement
            source = InplaceReplaceByIndex(source, sourceLength, entryEnd , entryEnd +1, ";}"); // +2 - 1
            // Build the string to insert
            int indexStringLength = (j == 0 ? 1 : (int)(log10(j)+1));
            char * replacementString = malloc(19 + indexStringLength + 1);
            replacementString[19 + indexStringLength + 1] = '\0';
            memcpy(replacementString, "if(index==", 10);
            sprintf(replacementString + 10, "%d", j);
            strcpy(replacementString + 10 + indexStringLength, "){return ");

            // Insert the correct index in the replacement string
            source = InplaceInsertByIndex(source, sourceLength, entryStart, replacementString);

            entryStart = entryEnd + 19 + indexStringLength + 2;
            free(replacementString);
        }

        // The replacement string is not needed anymore


        // Add The last "}" to close the function
        source = InplaceInsertByIndex(source, sourceLength, entryStart, "}");
        // Add the argument section of the function
        source = InplaceReplaceByIndex(source, sourceLength, variableNameStop, startArray , "(int index){");
        // Remove the []
        source = InplaceReplaceByIndex(source, sourceLength, typeStop, variableNameStart - 1, " ");
        // Finally, remove the "const" keyword
        source = InplaceReplaceByIndex(source, sourceLength, startPoint, startPoint + 5, " ");

        // Now, we have to turn every array access to a function call
        // TODO change the start position to be more accurate to the end of the function !
        for(int k = strstrPos(source + endArray, variableName) + endArray; k < strlen(source); ){
            int startAccess = GetNextTokenPosition(source, k, '[', "");
            int endAccess = GetClosingTokenPosition(source, startAccess);
            source = InplaceReplaceByIndex(source, sourceLength, endAccess, endAccess, ")");
            source = InplaceReplaceByIndex(source, sourceLength, startAccess, startAccess, "(");

            int nextPos = strstrPos(source + k, variableName) + k;
            if(nextPos == k) break;
            k = nextPos;
        }

        free(variableName);

    }else{
        // Nothing, go to the next loop iteration
    }

    return source;
}

int FindPositionAfterVersion(char * source){
    const char * position = FindString(source, "#version");
    if (position == NULL) return 0;
    for(int i=7; 1; ++i){
        if(position[i] == '\n'){
            return i;
        }
    }
}

char* FindAndCorrect(char* source, int* length, int mode) {
   const char*     template = mode == MODE_SWITCH ? switch_template : mode == MODE_CASE ? case_template : NULL;
   char*           scan_source = source;
   char            template_string[VARIABLE_SIZE];
   size_t          string_offset;
   size_t          offset = 0;
   unsigned char   rewind = 0;
   while(1) {
      int scan_result = sscanf(scan_source, template, &string_offset, &template_string);
      if(scan_result == 0) {
         scan_source++;
         continue;
      }else if(scan_result == EOF) {
         break;
      }
      offset = string_offset + (strstr(scan_source, mode == MODE_SWITCH ? "{" : mode == MODE_CASE ? ":" : 0) - scan_source); // find it by hand cause sscanf has trouble with two %n operators
      string_offset += (scan_source - source); // convert it from relative to scan to relative to base
      if(mode == MODE_SWITCH && !strstr(template_string, "int(") ) { // already cast to int, skip
         size_t insert_end_offset = string_offset + strlen(template_string);
         source = InplaceInsertByIndex(source, length, insert_end_offset, ")");
         source = InplaceInsertByIndex(source, length, string_offset, "int(");
         rewind = 1;
      }
      if(mode == MODE_CASE) {
         if(!isdigit(template_string[0])) { // cant have a number without the first digit, and the standard doesnt permit variable names starting with numbers
            char   decltemplate_formatted[VARIABLE_SIZE];
            float  declared_value = 99;
            snprintf(decltemplate_formatted, VARIABLE_SIZE, declaration_template, template_string, "%f");
            SHUT_LOGD("Scanning with template %s\n", decltemplate_formatted);
            char* scanbase = source;
            while(1) {
               int result = sscanf(scanbase, decltemplate_formatted, &declared_value);
               if(result == 0) {
                  scanbase++;
                  continue;
               }else if(result == EOF) {
                  SHUT_LOGD("Scanned the whole shader and didn't find declaration for %s with template \"%s\"\n", template_string, decltemplate_formatted);
                  abort();
               }
              break;
            }
            char   integer[VARIABLE_SIZE];
            snprintf(integer, VARIABLE_SIZE, "%i", (int)declared_value);
            size_t replace_end_offset = string_offset + strlen(template_string)-1;
            source = InplaceReplaceByIndex(source, length, string_offset, replace_end_offset, integer);
            rewind = 1;
         }
      }
      if(rewind) {
         scan_source = source; // since inplace replacement operations are destructive, the scan will be rewound after doing them
         rewind = 0;
      }else scan_source += offset;
   }
   return source;
}

char* ProcessSwitchCases(char* source, int* length) {
   source = FindAndCorrect(source, length, MODE_SWITCH);
   source = FindAndCorrect(source, length, MODE_CASE);
   return source;
}

void trim(char* str) {
    char* end;
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return;
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    *(end + 1) = 0;
}

// 辅助函数：解析并提取浮点数数组（用于 mat2, mat3, mat4, vec2, vec3, vec4 类型的处理）
int parse_floats_from_string(const char* str, GLfloat* outValues, int maxCount) {
    int count = 0;
    const char* cursor = str;

    while (*cursor && count < maxCount) {
        // 查找数字
        while (*cursor && !isdigit((unsigned char)*cursor) && *cursor != '-') cursor++;

        if (*cursor) {
            outValues[count++] = strtof(cursor, (char**)&cursor);
        }
    }
    return count;
}

// 辅助函数：解析 bool 类型
int parse_bool_from_string(const char* str) {
    if (strcmp(str, "true") == 0) {
        return GL_TRUE;
    }
    if (strcmp(str, "false") == 0) {
        return GL_FALSE;
    }
    return -1;  // 无效的布尔值
}

bool has_valid_data(char arr[256]) {
    for (int i = 0; i < 256; i++) {
        if (arr[i] != '\0' && arr[i] != '\n' && arr[i] != ' ') {
            return true;
        }
    }
    return false;
}

void set_uniforms_default_value(GLuint program, uniforms_declarations uniformVector, int uniformCount) {
    for (int i = 0; i < uniformCount; i++) {
        uniform_declaration_s* uniform = &uniformVector[i];
        if (!has_valid_data(uniform->variable) || !has_valid_data(uniform->initial_value))
        {
            break;
        }
        GLint location = gl4es_glGetUniformLocation(program, uniform->variable);

        if (location == -1) {
            SHUT_LOGD("Uniform variable %s not found in shader program.\n", uniform->variable);
            continue;
        }

        if (strstr(uniform->initial_value, "mat4") != NULL) {
            GLfloat matValues[16];
            int count = parse_floats_from_string(uniform->initial_value, matValues, 16);

            if (count == 16) {
                gl4es_glUniformMatrix4fv(location, 1, GL_FALSE, matValues);
            }
            else {
                SHUT_LOGD("Invalid mat4 initial value for uniform %s\n", uniform->variable);
            }
        }
        else if (strstr(uniform->initial_value, "mat3") != NULL) {
            GLfloat matValues[9];
            int count = parse_floats_from_string(uniform->initial_value, matValues, 9);

            if (count == 9) {
                gl4es_glUniformMatrix3fv(location, 1, GL_FALSE, matValues);
            }
            else {
                SHUT_LOGD("Invalid mat3 initial value for uniform %s\n", uniform->variable);
            }
        }
        else if (strstr(uniform->initial_value, "mat2") != NULL) {
            GLfloat matValues[4];
            int count = parse_floats_from_string(uniform->initial_value, matValues, 4);

            if (count == 4) {
                gl4es_glUniformMatrix2fv(location, 1, GL_FALSE, matValues);
            }
            else {
                SHUT_LOGD("Invalid mat2 initial value for uniform %s\n", uniform->variable);
            }
        }
        else if (strstr(uniform->initial_value, "vec4") != NULL) {
            GLfloat vecValues[4];
            int count = parse_floats_from_string(uniform->initial_value, vecValues, 4);

            if (count == 4) {
                gl4es_glUniform4fv(location, 1, vecValues);
            }
            else {
                SHUT_LOGD("Invalid vec4 initial value for uniform %s\n", uniform->variable);
            }
        }
        else if (strstr(uniform->initial_value, "vec3") != NULL) {
            // 处理 vec3 类型
            GLfloat vecValues[3];
            int count = parse_floats_from_string(uniform->initial_value, vecValues, 3);

            if (count == 3) {
                gl4es_glUniform3fv(location, 1, vecValues);
            }
            else {
                SHUT_LOGD("Invalid vec3 initial value for uniform %s\n", uniform->variable);
            }
        }
        else if (strstr(uniform->initial_value, "vec2") != NULL) {
            GLfloat vecValues[2];
            int count = parse_floats_from_string(uniform->initial_value, vecValues, 2);

            if (count == 2) {
                gl4es_glUniform2fv(location, 1, vecValues);
            }
            else {
                SHUT_LOGD("Invalid vec2 initial value for uniform %s\n", uniform->variable);
            }
        }
        else if (strstr(uniform->initial_value, "float") != NULL) {
            GLfloat value = strtof(uniform->initial_value, NULL);
            gl4es_glUniform1f(location, value);
        }
        else if (strstr(uniform->initial_value, "int") != NULL) {
            GLint value = strtol(uniform->initial_value, NULL, 10);
            gl4es_glUniform1i(location, value);
        }
        else if (strstr(uniform->initial_value, "bool") != NULL) {
            GLint value = parse_bool_from_string(uniform->initial_value);
            if (value != -1) {
                gl4es_glUniform1i(location, value);
            }
            else {
                SHUT_LOGD("Invalid bool initial value for uniform %s\n", uniform->variable);
            }
        }
        else if (strstr(uniform->initial_value, "sampler2D") != NULL) {
            gl4es_glUniform1i(location, 0);
        }
        else {
            SHUT_LOGE("[ERROR] Unsupported uniform type or invalid initial value for uniform %s\n", uniform->variable);
        }
    }
}

int startsWith(char* str, char* prefix) {
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

char* process_uniform_declarations(char* glslCode, uniforms_declarations uniformVector, int* uniformCount) {
    char* cursor = glslCode;
    char name[256], type[256], initial_value[1024];
    int modifiedCodeIndex = 0;
    size_t maxLength = 1024 * 10;
    char* modifiedGlslCode = (char*)malloc(maxLength * sizeof(char));
    if (!modifiedGlslCode) return NULL;

    while (*cursor) {
        if (strncmp(cursor, "uniform", 7) == 0) {
            char* cursor_start = cursor;
            
            cursor += 7;

            while (isspace((unsigned char)*cursor)) cursor++;

            // may be precision qualifier
            char* precision = NULL;
            if (startsWith(cursor, "highp")) {
                precision = " highp";
                cursor += 5;
                while (isspace((unsigned char)*cursor)) cursor++;
            } else if (startsWith(cursor, "lowp")) {
                precision = " lowp";
                cursor += 4;
                while (isspace((unsigned char)*cursor)) cursor++;
            } else if (startsWith(cursor, "mediump")) {
                precision = " mediump";
                cursor += 7;
                while (isspace((unsigned char)*cursor)) cursor++;
            }

            int i = 0;
            while (isalnum((unsigned char)*cursor) || *cursor == '_') {
                type[i++] = *cursor++;
            }
            type[i] = '\0';

            while (isspace((unsigned char)*cursor)) cursor++;

            // may be precision qualifier
            if(!precision)
            {
                if (startsWith(cursor, "highp")) {
                    precision = " highp";
                    cursor += 5;
                    while (isspace((unsigned char)*cursor)) cursor++;
                } else if (startsWith(cursor, "lowp")) {
                    precision = " lowp";
                    cursor += 4;
                    while (isspace((unsigned char)*cursor)) cursor++;
                } else if (startsWith(cursor, "mediump")) {
                    precision = " mediump";
                    cursor += 7;
                    while (isspace((unsigned char)*cursor)) cursor++;
                } else {
                    precision = "";
                }
            }
            
            while (isspace((unsigned char)*cursor)) cursor++;

            i = 0;
            while (isalnum((unsigned char)*cursor) || *cursor == '_') {
                name[i++] = *cursor++;
            }
            name[i] = '\0';
            while (isspace((unsigned char)*cursor)) cursor++;

            initial_value[0] = '\0';
            if (*cursor == '=') {
                cursor++;
                i = 0;
                while (*cursor && *cursor != ';') {
                    initial_value[i++] = *cursor++;
                }
                initial_value[i] = '\0';
                trim(initial_value);
            }

            strcpy(uniformVector[*uniformCount].variable, name);
            strcpy(uniformVector[*uniformCount].initial_value, initial_value);
            (*uniformCount)++;

            while (*cursor != ';' && *cursor) {
                cursor++;
            }
            
            char* cursor_end = cursor;

            int spaceLeft = maxLength - modifiedCodeIndex;
            int len = 0;

            if (*initial_value) {
                len = snprintf(modifiedGlslCode + modifiedCodeIndex, spaceLeft, "uniform%s %s %s;", precision, type, name);
            } else {
                // use original declaration
                size_t length = cursor_end - cursor_start + 1;
                if (length < spaceLeft) {
                    memcpy(modifiedGlslCode + modifiedCodeIndex, cursor_start, length);
                    len = (int)length;
                } else {
                    fprintf(stderr, "Error: Not enough space in buffer\n");
                }
                // len = snprintf(modifiedGlslCode + modifiedCodeIndex, spaceLeft, "uniform%s %s %s;", precision, type, name);
            }

            if (len < 0 || len >= spaceLeft) {
                free(modifiedGlslCode);
                return NULL;
            }
            modifiedCodeIndex += len;

            while (*cursor == ';') cursor++;

        } else {
            modifiedGlslCode[modifiedCodeIndex++] = *cursor++;
        }

        if (modifiedCodeIndex >= maxLength - 1) {
            maxLength *= 2;
            modifiedGlslCode = (char*)realloc(modifiedGlslCode, maxLength);
            if (!modifiedGlslCode) return NULL;
        }
    }

    modifiedGlslCode[modifiedCodeIndex] = '\0';
    return modifiedGlslCode;
}

typedef struct {
    const char* glname;
    const char* name;
    const char* type;
    const char* prec;
    int attrib;
} builtin_attrib_t;

const builtin_attrib_t builtin_attrib[] = {
    {"gl_Vertex", "_gl4es_Vertex", "vec4", "highp", ARB_VERTEX},
    {"gl_Color", "_gl4es_Color", "vec4", "lowp", ARB_COLOR},
    {"gl_MultiTexCoord0", "_gl4es_MultiTexCoord0", "vec4", "highp", ARB_MULTITEXCOORD0},
    {"gl_MultiTexCoord1", "_gl4es_MultiTexCoord1", "vec4", "highp", ARB_MULTITEXCOORD1},
    {"gl_MultiTexCoord2", "_gl4es_MultiTexCoord2", "vec4", "highp", ARB_MULTITEXCOORD2},
    {"gl_MultiTexCoord3", "_gl4es_MultiTexCoord3", "vec4", "highp", ARB_MULTITEXCOORD3},
    {"gl_MultiTexCoord4", "_gl4es_MultiTexCoord4", "vec4", "highp", ARB_MULTITEXCOORD4},
    {"gl_MultiTexCoord5", "_gl4es_MultiTexCoord5", "vec4", "highp", ARB_MULTITEXCOORD5},
    {"gl_MultiTexCoord6", "_gl4es_MultiTexCoord6", "vec4", "highp", ARB_MULTITEXCOORD6},
    {"gl_MultiTexCoord7", "_gl4es_MultiTexCoord7", "vec4", "highp", ARB_MULTITEXCOORD7},
    {"gl_MultiTexCoord8", "_gl4es_MultiTexCoord8", "vec4", "highp", ARB_MULTITEXCOORD8},
    {"gl_MultiTexCoord9", "_gl4es_MultiTexCoord9", "vec4", "highp", ARB_MULTITEXCOORD9},
    {"gl_MultiTexCoord10", "_gl4es_MultiTexCoord10", "vec4", "highp", ARB_MULTITEXCOORD10},
    {"gl_MultiTexCoord11", "_gl4es_MultiTexCoord11", "vec4", "highp", ARB_MULTITEXCOORD11},
    {"gl_MultiTexCoord12", "_gl4es_MultiTexCoord12", "vec4", "highp", ARB_MULTITEXCOORD12},
    {"gl_MultiTexCoord13", "_gl4es_MultiTexCoord13", "vec4", "highp", ARB_MULTITEXCOORD13},
    {"gl_MultiTexCoord14", "_gl4es_MultiTexCoord14", "vec4", "highp", ARB_MULTITEXCOORD14},
    {"gl_MultiTexCoord15", "_gl4es_MultiTexCoord15", "vec4", "highp", ARB_MULTITEXCOORD15},
    {"gl_SecondaryColor", "_gl4es_SecondaryColor", "vec4", "lowp", ARB_SECONDARY},
    {"gl_Normal", "_gl4es_Normal", "vec3", "highp", ARB_NORMAL},
    {"gl_FogCoord", "_gl4es_FogCoord", "float", "highp", ARB_FOGCOORD}
};

const builtin_attrib_t builtin_attrib_compressed[] = {
    {"gl_Vertex", "_gl4es_Vertex", "vec4", "highp", COMP_VERTEX},
    {"gl_Color", "_gl4es_Color", "vec4", "lowp", COMP_COLOR},
    {"gl_MultiTexCoord0", "_gl4es_MultiTexCoord0", "vec4", "highp", COMP_MULTITEXCOORD0},
    {"gl_MultiTexCoord1", "_gl4es_MultiTexCoord1", "vec4", "highp", COMP_MULTITEXCOORD1},
    {"gl_MultiTexCoord2", "_gl4es_MultiTexCoord2", "vec4", "highp", COMP_MULTITEXCOORD2},
    {"gl_MultiTexCoord3", "_gl4es_MultiTexCoord3", "vec4", "highp", COMP_MULTITEXCOORD3},
    {"gl_MultiTexCoord4", "_gl4es_MultiTexCoord4", "vec4", "highp", COMP_MULTITEXCOORD4},
    {"gl_MultiTexCoord5", "_gl4es_MultiTexCoord5", "vec4", "highp", COMP_MULTITEXCOORD5},
    {"gl_MultiTexCoord6", "_gl4es_MultiTexCoord6", "vec4", "highp", COMP_MULTITEXCOORD6},
    {"gl_MultiTexCoord7", "_gl4es_MultiTexCoord7", "vec4", "highp", COMP_MULTITEXCOORD7},
    {"gl_MultiTexCoord8", "_gl4es_MultiTexCoord8", "vec4", "highp", COMP_MULTITEXCOORD8},
    {"gl_MultiTexCoord9", "_gl4es_MultiTexCoord9", "vec4", "highp", COMP_MULTITEXCOORD9},
    {"gl_MultiTexCoord10", "_gl4es_MultiTexCoord10", "vec4", "highp", COMP_MULTITEXCOORD10},
    {"gl_MultiTexCoord11", "_gl4es_MultiTexCoord11", "vec4", "highp", COMP_MULTITEXCOORD11},
    {"gl_MultiTexCoord12", "_gl4es_MultiTexCoord12", "vec4", "highp", COMP_MULTITEXCOORD12},
    {"gl_MultiTexCoord13", "_gl4es_MultiTexCoord13", "vec4", "highp", COMP_MULTITEXCOORD13},
    {"gl_MultiTexCoord14", "_gl4es_MultiTexCoord14", "vec4", "highp", COMP_MULTITEXCOORD14},
    {"gl_MultiTexCoord15", "_gl4es_MultiTexCoord15", "vec4", "highp", COMP_MULTITEXCOORD15},
    {"gl_SecondaryColor", "_gl4es_SecondaryColor", "vec4", "lowp", COMP_SECONDARY},
    {"gl_Normal", "_gl4es_Normal", "vec3", "highp", COMP_NORMAL},
    {"gl_FogCoord", "_gl4es_FogCoord", "float", "highp", COMP_FOGCOORD}
};

typedef struct {
    const char* glname;
    const char* name;
    const char* type;
    int   texarray;
    reserved_matrix_t matrix;
} builtin_matrix_t;

const builtin_matrix_t builtin_matrix[] = {
    {"gl_ModelViewMatrixInverseTranspose", "_gl4es_ITModelViewMatrix", "mat4", 0, MAT_MV_IT},
    {"gl_ModelViewMatrixInverse", "_gl4es_IModelViewMatrix", "mat4", 0, MAT_MV_I},
    {"gl_ModelViewMatrixTranspose", "_gl4es_TModelViewMatrix", "mat4", 0, MAT_MV_T},
    {"gl_ModelViewMatrix", "_gl4es_ModelViewMatrix", "mat4", 0, MAT_MV},
    {"gl_ProjectionMatrixInverseTranspose", "_gl4es_ITProjectionMatrix", "mat4", 0, MAT_P_IT},
    {"gl_ProjectionMatrixInverse", "_gl4es_IProjectionMatrix", "mat4", 0, MAT_P_I},
    {"gl_ProjectionMatrixTranspose", "_gl4es_TProjectionMatrix", "mat4", 0, MAT_P_T},
    {"gl_ProjectionMatrix", "_gl4es_ProjectionMatrix", "mat4", 0, MAT_P},
    {"gl_ModelViewProjectionMatrixInverseTranspose", "_gl4es_ITModelViewProjectionMatrix", "mat4", 0, MAT_MVP_IT},
    {"gl_ModelViewProjectionMatrixInverse", "_gl4es_IModelViewProjectionMatrix", "mat4", 0, MAT_MVP_I},
    {"gl_ModelViewProjectionMatrixTranspose", "_gl4es_TModelViewProjectionMatrix", "mat4", 0, MAT_MVP_T},
    {"gl_ModelViewProjectionMatrix", "_gl4es_ModelViewProjectionMatrix", "mat4", 0, MAT_MVP},
    // non standard version to avoid useless array of Matrix Uniform (in case the compiler as issue optimising this)
    {"gl_TextureMatrix_0", "_gl4es_TextureMatrix_0", "mat4", 0, MAT_T0},
    {"gl_TextureMatrix_1", "_gl4es_TextureMatrix_1", "mat4", 0, MAT_T1},
    {"gl_TextureMatrix_2", "_gl4es_TextureMatrix_2", "mat4", 0, MAT_T2},
    {"gl_TextureMatrix_3", "_gl4es_TextureMatrix_3", "mat4", 0, MAT_T3},
    {"gl_TextureMatrix_4", "_gl4es_TextureMatrix_4", "mat4", 0, MAT_T4},
    {"gl_TextureMatrix_5", "_gl4es_TextureMatrix_5", "mat4", 0, MAT_T5},
    {"gl_TextureMatrix_6", "_gl4es_TextureMatrix_6", "mat4", 0, MAT_T6},
    {"gl_TextureMatrix_7", "_gl4es_TextureMatrix_7", "mat4", 0, MAT_T7},
    {"gl_TextureMatrix_8", "_gl4es_TextureMatrix_8", "mat4", 0, MAT_T8},
    {"gl_TextureMatrix_9", "_gl4es_TextureMatrix_9", "mat4", 0, MAT_T9},
    {"gl_TextureMatrix_10", "_gl4es_TextureMatrix_10", "mat4", 0, MAT_T10},
    {"gl_TextureMatrix_11", "_gl4es_TextureMatrix_11", "mat4", 0, MAT_T11},
    {"gl_TextureMatrix_12", "_gl4es_TextureMatrix_12", "mat4", 0, MAT_T12},
    {"gl_TextureMatrix_13", "_gl4es_TextureMatrix_13", "mat4", 0, MAT_T13},
    {"gl_TextureMatrix_14", "_gl4es_TextureMatrix_14", "mat4", 0, MAT_T14},
    {"gl_TextureMatrix_15", "_gl4es_TextureMatrix_15", "mat4", 0, MAT_T15},
    // regular texture matrix
    {"gl_TextureMatrixInverseTranspose", "_gl4es_ITTextureMatrix", "mat4", 1, MAT_T0_IT},
    {"gl_TextureMatrixInverse", "_gl4es_ITextureMatrix", "mat4", 1, MAT_T0_I},
    {"gl_TextureMatrixTranspose", "_gl4es_TTextureMatrix", "mat4", 1, MAT_T0_T},
    {"gl_TextureMatrix", "_gl4es_TextureMatrix", "mat4", 1, MAT_T0},
    {"gl_NormalMatrix", "_gl4es_NormalMatrix", "mat3", 0, MAT_N}
  };

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
static const char* gl4es_MaxLightsSource =
"#define _gl4es_MaxLights " STR(MAX_LIGHT) "\n";
static const char* gl4es_MaxClipPlanesSource =
"#define _gl4es_MaxClipPlanes " STR(MAX_CLIP_PLANES) "\n";
static const char* gl4es_MaxTextureUnitsSource =
"#define _gl4es_MaxTextureUnits " STR(MAX_TEX) "\n";
static const char* gl4es_MaxTextureCoordsSource =
"#define _gl4es_MaxTextureCoords " STR(MAX_TEX) "\n";
#undef STR
#undef STR_HELPER

static const char* gl4es_LightSourceParametersSource = 
"struct gl_LightSourceParameters\n"
"{\n"
"   vec4 ambient;\n"
"   vec4 diffuse;\n"
"   vec4 specular;\n"
"   vec4 position;\n"
"   vec4 halfVector;\n"   //halfVector = normalize(normalize(position) + vec3(0,0,1) if vbs==FALSE)
"   vec3 spotDirection;\n"
"   float spotExponent;\n"
"   float spotCutoff;\n"
"   float spotCosCutoff;\n"
"   float constantAttenuation;\n"
"   float linearAttenuation;\n"
"   float quadraticAttenuation;\n"
"};\n"
"uniform gl_LightSourceParameters gl_LightSource[gl_MaxLights];\n";

static const char* gl4es_LightModelParametersSource =
"struct gl_LightModelParameters {\n"
"  vec4 ambient;\n"
"};\n"
"uniform gl_LightModelParameters gl_LightModel;\n";

static const char* gl4es_MaterialParametersSource =
"struct gl_MaterialParameters\n"
"{\n"
"   vec4 emission;\n"
"   vec4 ambient;\n"
"   vec4 diffuse;\n"
"   vec4 specular;\n"
"   float shininess;\n"
"};\n"
"uniform gl_MaterialParameters gl_FrontMaterial;\n"
"uniform gl_MaterialParameters gl_BackMaterial;\n";

static const char* gl4es_LightModelProductsSource =
"struct gl_LightModelProducts\n"
"{\n"
"   vec4 sceneColor;\n"
"};\n"
"uniform gl_LightModelProducts gl_FrontLightModelProduct;\n"
"uniform gl_LightModelProducts gl_BackLightModelProduct;\n";

static const char* gl4es_LightProductsSource =
"struct gl_LightProducts\n"
"{\n"
"   vec4 ambient;\n"
"   vec4 diffuse;\n"
"   vec4 specular;\n"
"};\n"
"uniform gl_LightProducts gl_FrontLightProduct[gl_MaxLights];\n"
"uniform gl_LightProducts gl_BackLightProduct[gl_MaxLights];\n";

static const char* gl4es_PointSpriteSource =
"struct gl_PointParameters\n"
"{\n"
"   float size;\n"
"   float sizeMin;\n"
"   float sizeMax;\n"
"   float fadeThresholdSize;\n"
"   float distanceConstantAttenuation;\n"
"   float distanceLinearAttenuation;\n"
"   float distanceQuadraticAttenuation;\n"
"};\n"
"uniform gl_PointParameters gl_Point;\n";

static const char* gl4es_FogParametersSource =
"struct gl_FogParameters {\n"
"    lowp vec4 color;\n"
"    mediump float density;\n"
"    mediump float start;\n"
"    mediump float end;\n"
"    mediump float scale;\n"   // Derived:   1.0 / (end - start) 
"};\n"
"uniform gl_FogParameters gl_Fog;\n";
static const char* gl4es_FogParametersSourceHighp =
"struct gl_FogParameters {\n"
"    lowp vec4 color;\n"
"    mediump float density;\n"
"    highp   float start;\n"
"    highp   float end;\n"
"    highp   float scale;\n"   // Derived:   1.0 / (end - start) 
"};\n"
"uniform gl_FogParameters gl_Fog;\n";

static const char* gl4es_texenvcolorSource =
"uniform vec4 gl_TextureEnvColor[gl_MaxTextureUnits];\n";

static const char* gl4es_texgeneyeSource[4] = {
"uniform vec4 gl_EyePlaneS[gl_MaxTextureCoords];\n",
"uniform vec4 gl_EyePlaneT[gl_MaxTextureCoords];\n",
"uniform vec4 gl_EyePlaneR[gl_MaxTextureCoords];\n",
"uniform vec4 gl_EyePlaneQ[gl_MaxTextureCoords];\n" };

static const char* gl4es_texgenobjSource[4] = {
"uniform vec4 gl_ObjectPlaneS[gl_MaxTextureCoords];\n",
"uniform vec4 gl_ObjectPlaneT[gl_MaxTextureCoords];\n",
"uniform vec4 gl_ObjectPlaneR[gl_MaxTextureCoords];\n",
"uniform vec4 gl_ObjectPlaneQ[gl_MaxTextureCoords];\n" };

static const char* gl4es_clipplanesSource = 
"uniform vec4  gl_ClipPlane[gl_MaxClipPlanes];\n";

static const char* gl4es_normalscaleSource =
"uniform float gl_NormalScale;\n";

static const char* gl4es_instanceID =
"#define GL_ARB_draw_instanced 1\n"
"uniform int _gl4es_InstanceID;\n";

static const char* gl4es_frontColorSource =
"varying lowp vec4 _gl4es_FrontColor;\n";

static const char* gl4es_backColorSource =
"varying lowp vec4 _gl4es_BackColor;\n";

static const char* gl4es_frontSecondaryColorSource =
"varying lowp vec4 _gl4es_FrontSecondaryColor;\n";

static const char* gl4es_backSecondaryColorSource =
"varying lowp vec4 _gl4es_BackSecondaryColor;\n";

static const char* gl4es_texcoordSource =
"varying mediump vec4 _gl4es_TexCoord[%d];\n";

static const char* gl4es_texcoordSourceAlt =
"varying mediump vec4 _gl4es_TexCoord_%d;\n";

static const char* gl4es_fogcoordSource =
"varying mediump float _gl4es_FogFragCoord;\n";

static const char* gl4es_ftransformSource = 
"\n"
"highp vec4 ftransform() {\n"
" return gl_ModelViewProjectionMatrix * gl_Vertex;\n"
"}\n";

static const char* gl4es_dummyClipVertex = 
"vec4 dummyClipVertex_%d";

static const char* gl_TexCoordSource = "gl_TexCoord[";

static const char* gl_TexMatrixSources[] = {
"gl_TextureMatrixInverseTranspose[",
"gl_TextureMatrixInverse[",
"gl_TextureMatrixTranspose[",
"gl_TextureMatrix["
};

static const char* GLESHeader[] = {
  "#version 100\n%s",
  "#version 300 es\n%s",
  "#version 310 es\n%s",
  "#version 320 es\n%s",
};

static const char* gl4es_transpose =
"mat2 gl4es_transpose(mat2 m) {\n"
" return mat2(m[0][0], m[0][1],\n"
"             m[1][0], m[1][1]);\n"
"}\n"
"mat3 gl4es_transpose(mat3 m) {\n"
" return mat3(m[0][0], m[0][1], m[0][2],\n"
"             m[1][0], m[1][1], m[1][2],\n"
"             m[2][0], m[2][1], m[2][2]);\n"
"}\n"
"mat4 gl4es_transpose(mat4 m) {\n"
" return mat4(m[0][0], m[0][1], m[0][2], m[0][3],\n"
"             m[1][0], m[1][1], m[1][2], m[1][3],\n"
"             m[2][0], m[2][1], m[2][2], m[2][3],\n"
"             m[3][0], m[3][1], m[3][2], m[3][3]);\n"
"}\n";

static const char* HackAltPow = 
"\n";
static const char* HackAltClamp = 
"\n";


static const char* HackAltMax = 
"\n";
static const char* HackAltMin = 
/*
"float min(float a, int b) {\n"
" return min(a, float(b));\n"
"}\n"
"float min(int a, float b) {\n"
" return min(float(a), b);\n"
"}\n";
*/
"\n";

static const char* HackAltMod = 
/*
"float mod(float f, int a) {\n"
" return mod(f, float(a));\n"
"}\n"
"vec2 mod(vec2 f, int a) {\n"
" return mod(f, float(a));\n"
"}\n"
"vec3 mod(vec3 f, int a) {\n"
" return mod(f, float(a));\n"
"}\n"
"vec4 mod(vec4 f, int a) {\n"
" return mod(f, float(a));\n"
"}\n";
*/
"\n";

/*
static const char* texture2DLodAlt =
"vec4 _gl4es_texture2DLod(sampler2D sampler, vec2 coord, float lod) {\n"
" return texture2DLod(sampler, coord, lod);\n"
"}\n";

static const char* texture2DProjLodAlt =
"vec4 _gl4es_texture2DProjLod(sampler2D sampler, vec3 coord, float lod) {\n"
" return texture2DProjLod(sampler, coord, lod);\n"
"}\n"
"vec4 _gl4es_texture2DProjLod(sampler2D sampler, vec4 coord, float lod) {\n"
" return texture2DProjLod(sampler, coord, lod);\n"
"}\n";
static const char* textureCubeLodAlt =
"vec4 _gl4es_textureCubeLod(samplerCube sampler, vec3 coord, float lod) {\n"
" return textureCubeLod(sampler, coord, lod);\n"
"}\n";

static const char* texture2DGradAlt =
"vec4 _gl4es_texture2DGrad(sampler2D sampler, vec2 coord, vec2 dPdx, vec2 dPdy) {\n"
" return texture2DGrad(sampler, coord, dPdx, dPdy);\n"
"}\n";

static const char* texture2DProjGradAlt =
"vec4 _gl4es_texture2DProjGrad(sampler2D sampler, vec3 coord, vec2 dPdx, vec2 dPdy) {\n"
" return texture2DProjGrad(sampler, coord, dPdx, dPdy);\n"
"}\n"
"vec4 _gl4es_texture2DProjGrad(sampler2D sampler, vec4 coord, vec2 dPdx, vec2 dPdy) {\n"
" return texture2DProjGrad(sampler, coord, dPdx, dPdy);\n"
"}\n";
static const char* textureCubeGradAlt =
"vec4 _gl4es_textureCubeGrad(samplerCube sampler, vec3 coord, vec2 dPdx, vec2 dPdy) {\n"
" return textureCubeGrad(sampler, coord, dPdx, dPdy);\n"
"}\n";
*/

/*
static const char* useEXTDrawBuffers =
"#extension GL_EXT_draw_buffers : enable\n";
*/

static const char* gl_ProgramEnv  = "gl_ProgramEnv";
static const char* gl_ProgramLocal= "gl_ProgramLocal";

static const char* gl_Samplers1D = "gl_Sampler1D_";
static const char* gl_Samplers2D = "gl_Sampler2D_";
static const char* gl_Samplers3D = "gl_Sampler3D_";
static const char* gl_SamplersCube = "gl_SamplerCube_";
static const char* gl4es_Samplers1D = "_gl4es_Sampler1D_";
static const char* gl4es_Samplers2D = "_gl4es_Sampler2D_";
static const char* gl4es_Samplers3D = "_gl4es_Sampler3D_";
static const char* gl4es_SamplersCube = "_gl4es_SamplerCube_";
static const char* gl4es_Samplers1D_uniform = "uniform sampler2D _gl4es_Sampler1D_";
static const char* gl4es_Samplers2D_uniform = "uniform sampler2D _gl4es_Sampler2D_";
static const char* gl4es_Samplers3D_uniform = "uniform sampler2D _gl4es_Sampler3D_";
static const char* gl4es_SamplersCube_uniform = "uniform samplerCube _gl4es_SamplerCube_";

static const char* gl_VertexAttrib = "gl_VertexAttrib_";
static const char* gl4es_VertexAttrib = "_gl4es_VertexAttrib_";

char gl_VA[MAX_VATTRIB][32] = {0};
char gl4es_VA[MAX_VATTRIB][32] = {0};


int FindPositionAfterDirectives(char * source){
    const char * position = FindString(source, "#version");
    if (position == NULL) return 0;
    for(int i=7; 1; ++i){
        if(position[i] == '\n'){
            if(position[i+1] == '#') continue; // a directive is present right after, skip
            return i;
        }
    }
}

unsigned long strstrPos(const char * haystack, const char * needle){
    char * substr = strstr(haystack, needle);
    if (substr == NULL) return 0;
    return (substr - haystack);
}

char * ReplacePrecisionQualifiers(char * source, int * sourceLength, int isVertex){

    if(!doesShaderVersionContainsES(source)){
        if (globals4es.vgpu_dump) {
            printf("\nSKIPPING the replacement qualifiers step\n");
        }
        return source;
    }

    // Step 1 is to remove any "precision" qualifiers
    for(unsigned long currentPosition=strstrPos(source, "precision "); currentPosition>0;currentPosition=strstrPos(source, "precision ")){
        // Once a qualifier is found, get to the end of the instruction and replace
        int endPosition = GetNextTokenPosition(source, currentPosition, ';', "");
        source = InplaceReplaceByIndex(source, sourceLength, currentPosition, endPosition,"");
    }

    // Step 2 is to insert precision qualifiers, even the ones we think are defaults, since there are defaults only for some types

    int insertPoint = FindPositionAfterDirectives(source);
    source = InplaceInsertByIndex(source, sourceLength, insertPoint,
                                   "\nprecision lowp sampler2D;\n"
                                   "precision lowp sampler3D;\n"
                                   "precision lowp sampler2DShadow;\n"
                                   "precision lowp samplerCubeShadow;\n"
                                   "precision lowp sampler2DArray;\n"
                                   "precision lowp sampler2DArrayShadow;\n"
                                   "precision lowp samplerCube;\n"
                                   "#ifdef GL_EXT_texture_buffer\n"
                                   "precision lowp samplerBuffer;\n"
                                   "precision lowp imageBuffer;\n"
                                   "#endif\n"
                                   "#ifdef GL_EXT_texture_cube_map_array\n"
                                   "precision lowp imageCubeArray;\n"
                                   "precision lowp samplerCubeArray;\n"
                                   "precision lowp samplerCubeArrayShadow;\n"
                                   "#endif\n"
                                   "#ifdef GL_OES_texture_storage_multisample_2d_array\n"
                                   "precision lowp sampler2DMS;\n"
                                   "precision lowp sampler2DMSArray;\n"
                                   "#endif\n");

    if(GetShaderVersion(source) > 300){
        source = InplaceInsertByIndex(source, sourceLength,insertPoint,
                                      "\nprecision lowp image2D;\n"
                                      "precision lowp image2DArray;\n"
                                      "precision lowp image3D;\n"
                                      "precision lowp imageCube;\n");
    }
    int supportHighp = ((isVertex || hardext.highp) ? 1 : 0);
    source = InplaceInsertByIndex(source, sourceLength, insertPoint, supportHighp ? "\nprecision highp float;\n" : "\nprecision medium float;\n");

    if (globals4es.vgpu_precision != 0){
        char * target_precision;
        switch (globals4es.vgpu_precision) {
            case 1: target_precision = "highp"; break;
            case 2: target_precision = "mediump"; break;
            case 3: target_precision = "lowp"; break;
            default: target_precision = "highp";
        }
        source = ReplaceVariableName(source, sourceLength, "highp", target_precision);
        source = ReplaceVariableName(source, sourceLength, "mediump", target_precision);
        source = ReplaceVariableName(source, sourceLength, "lowp", target_precision);
    }

    return source;
}

int GetShaderVersion(const char * source){
    // Oh yeah, I won't care much about this function
    if(FindString(source, "#version 320 es")){return 320;}
    if(FindString(source, "#version 310 es")){return 310;}
    if(FindString(source, "#version 300 es")){return 300;}
    if(FindString(source, "#version 320")){return 320;}
    if(FindString(source, "#version 330")){return 330;}
    if(FindString(source, "#version 400")){return 400;}
    if(FindString(source, "#version 410")){return 410;}
    if(FindString(source, "#version 420")){return 420;}
    if(FindString(source, "#version 430")){return 430;}
    if(FindString(source, "#version 440")){return 440;}
    if(FindString(source, "#version 450")){return 450;}
    if(FindString(source, "#version 460")){return 460;}
    if(FindString(source, "#version 150")){return 150;}
    if(FindString(source, "#version 130")){return 130;}
    if(FindString(source, "#version 110")){return 110;}
    if(FindString(source, "#version 120")){return 120;}
    return 100;
}

int GetNextTokenPosition(const char * source, int initialPosition, const char token, const char * acceptedChars){
    for(int i=initialPosition+1; i< strlen(source); ++i){
        // Tripping check
        if(strlen(acceptedChars) > 0){
            for(int j=0; j< strlen(acceptedChars); ++j){
                if (source[i] == acceptedChars[j]) break; // No tripping, continue
            }
            return initialPosition; // Tripped, meaning the token is not found
        }

        if (source[i] == token){
            return i;
        }
    }
    return initialPosition;
}

char * ExtractString(char * source, int startString, int endString){
    char * subString = malloc((endString - startString) +1);
    subString[(endString - startString) +1] = '\0';
    memcpy(subString, source + startString, (endString - startString));
    return subString;
}

/**
 * Replace the out vec4 from a fragment shader by the gl_FragColor constant
 * @param source The shader as a string
 * @return The shader, maybe in a different memory location
 */
char * ReplaceFragmentOut(char * source, int *sourceLength){
    int startPosition = strstrPos(source, "out");
    if(startPosition == 0) return source; // No "out" keyword
    int t1, t2;
    GetNextWord(source, startPosition, &t1, &t2); // Catches "out"
    GetNextWord(source, t2, &t1, &t2); // Catches "vec4"
    GetNextWord(source, t2, &t1, &t2); // Catches the variableName

    // Load the variable inside another string
    char * variableName = malloc(t2 - t1 + 1);
    variableName[t2 - t1 + 1] = '\0';
    memcpy(variableName, source + t1, t2 - t1);

    // Removing the declaration
    source = InplaceReplaceByIndex(source, sourceLength, startPosition, t2 + 1, "");

    // Replacing occurrences of the variable
    source = ReplaceVariableName(source, sourceLength, variableName, "gl_FragColor");

    free(variableName);

    return source;
}

/**
 * Get to the start, then end of the next of current word.
 * @param source The shader as a string
 * @param startPoint The start point to look at
 * @param startWord Will point to the start of the word
 * @param endWord Will point to the end of the word
 */
void GetNextWord(char *source, int startPoint, int * startWord, int * endWord){
    // Step 1: Find the real start point
    int start = 0;
    while(!start){
        if(isValidFunctionName(source[startPoint] ) || isDigit(source[startPoint])){
            start = startPoint;
            break;
        }
        ++startPoint;
    }

    // Step 2: Find the end of a word
    int end = 0;
    while (!end){
        if(!isValidFunctionName(source[startPoint] ) && !isDigit(source[startPoint])){
            end = startPoint;
            break;
        }
        ++startPoint;
    }

    // Then return values
    *startWord = start;
    *endWord = end;
}

char * InsertExtensions(char *source, int *sourceLength){
    int insertPoint = FindPositionAfterDirectives(source);
    //printf("INSERT POINT: %i\n", insertPoint);

    source = InsertExtension(source, sourceLength, insertPoint+1, "GL_EXT_shader_non_constant_global_initializers");
    source = InsertExtension(source, sourceLength, insertPoint+1, "GL_EXT_texture_cube_map_array");
    source = InsertExtension(source, sourceLength, insertPoint+1, "GL_EXT_texture_buffer");
    source = InsertExtension(source, sourceLength, insertPoint+1, "GL_OES_texture_storage_multisample_2d_array");
    return source;
}

char * InsertExtension(char * source, int * sourceLength, const int insertPoint, const char * extension){
    // First, insert the model, then the extension
    source = InplaceInsertByIndex(source, sourceLength, insertPoint, "#ifdef __EXT__ \n#extension __EXT__ : enable\n#endif\n");
    source = InplaceReplaceSimple(source, sourceLength, "__EXT__", extension);
    return source;
}

int doesShaderVersionContainsES(const char * source){
    return GetShaderVersion(source) >= 300;
}

char * WrapIvecFunctions(char * source, int * sourceLength){
    source = WrapFunction(source, sourceLength, "texelFetch", "vgpu_texelFetch", "\nvec4 vgpu_texelFetch(sampler2D sampler, vec2 P, float lod){return texelFetch(sampler, ivec2(int(P.x), int(P.y)), int(lod));}\n"
                                                                                 "vec4 vgpu_texelFetch(sampler3D sampler, vec3 P, float lod){return texelFetch(sampler, ivec3(int(P.x), int(P.y), int(P.z)), int(lod));}\n"
                                                                                 "vec4 vgpu_texelFetch(sampler2DArray sampler, vec3 P, float lod){return texelFetch(sampler, ivec3(int(P.x), int(P.y), int(P.z)), int(lod));}\n"
                                                                                 "#ifdef GL_EXT_texture_buffer\n"
                                                                                 "vec4 vgpu_texelFetch(samplerBuffer sampler, float P){return texelFetch(sampler, int(P));}\n"
                                                                                 "#endif\n"
                                                                                 "#ifdef GL_OES_texture_storage_multisample_2d_array\n"
                                                                                 "vec4 vgpu_texelFetch(sampler2DMS sampler, vec2 P, float _sample){return texelFetch(sampler, ivec2(int(P.x), int(P.y)), int(_sample));}\n"
                                                                                 "vec4 vgpu_texelFetch(sampler2DMSArray sampler, vec3 P, float _sample){return texelFetch(sampler, ivec3(int(P.x), int(P.y), int(P.z)), int(_sample));}\n"
                                                                                 "#endif\n");
    source = WrapFunction(source, sourceLength, "textureSize", "vgpu_textureSize", "\nvec2 vgpu_textureSize(sampler2D sampler, float lod){ivec2 size = textureSize(sampler, int(lod));return vec2(size.x, size.y);}\n"
                                                                                   "vec3 vgpu_textureSize(sampler3D sampler, float lod){ivec3 size = textureSize(sampler, int(lod));return vec3(size.x, size.y, size.z);}\n"
                                                                                   "vec2 vgpu_textureSize(samplerCube sampler, float lod){ivec2 size = textureSize(sampler, int(lod));return vec2(size.x, size.y);}\n"
                                                                                   "vec2 vgpu_textureSize(sampler2DShadow sampler, float lod){ivec2 size = textureSize(sampler, int(lod));return vec2(size.x, size.y);}\n"
                                                                                   "vec2 vgpu_textureSize(samplerCubeShadow sampler, float lod){ivec2 size = textureSize(sampler, int(lod));return vec2(size.x, size.y);}\n"
                                                                                   "#ifdef GL_EXT_texture_cube_map_array\n"
                                                                                   "vec3 vgpu_textureSize(samplerCubeArray sampler, float lod){ivec3 size = textureSize(sampler, int(lod));return vec3(size.x, size.y, size.z);}\n"
                                                                                   "vec3 vgpu_textureSize(samplerCubeArrayShadow sampler, float lod){ivec3 size = textureSize(sampler, int(lod));return vec3(size.x, size.y, size.z);}\n"
                                                                                   "#endif\n"
                                                                                   "vec3 vgpu_textureSize(sampler2DArray sampler, float lod){ivec3 size = textureSize(sampler, int(lod));return vec3(size.x, size.y, size.z);}\n"
                                                                                   "vec3 vgpu_textureSize(sampler2DArrayShadow sampler, float lod){ivec3 size = textureSize(sampler, int(lod));return vec3(size.x, size.y, size.z);}\n"
                                                                                   "#ifdef GL_EXT_texture_buffer\n"
                                                                                   "float vgpu_textureSize(samplerBuffer sampler){return float(textureSize(sampler));}\n"
                                                                                   "#endif\n"
                                                                                   "#ifdef GL_OES_texture_storage_multisample_2d_array\n"
                                                                                   "vec2 vgpu_textureSize(sampler2DMS sampler){ivec2 size = textureSize(sampler);return vec2(size.x, size.y);}\n"
                                                                                   "vec3 vgpu_textureSize(sampler2DMSArray sampler){ivec3 size = textureSize(sampler);return vec3(size.x, size.y, size.z);}\n"
                                                                                   "#endif\n");

    source = WrapFunction(source, sourceLength, "textureOffset", "vgpu_textureOffset", "\nvec4 vgpu_textureOffset(sampler2D tex, vec2 P, vec2 offset, float bias){ivec2 Size = textureSize(tex, 0);return texture(tex, P+offset/vec2(float(Size.x), float(Size.y)), bias);}\n"
                                                                                       "vec4 vgpu_textureOffset(sampler2D tex, vec2 P, vec2 offset){return vgpu_textureOffset(tex, P, offset, 0.0);}\n"
                                                                                       "vec4 vgpu_textureOffset(sampler3D tex, vec3 P, vec3 offset, float bias){ivec3 Size = textureSize(tex, 0);return texture(tex, P+offset/vec3(float(Size.x), float(Size.y), float(Size.z)), bias);}\n"
                                                                                       "vec4 vgpu_textureOffset(sampler3D tex, vec3 P, vec3 offset){return vgpu_textureOffset(tex, P, offset, 0.0);}\n"
                                                                                       "float vgpu_textureOffset(sampler2DShadow tex, vec3 P, vec2 offset, float bias){ivec2 Size = textureSize(tex, 0);return texture(tex, P+vec3(offset.x, offset.y, 0)/vec3(float(Size.x), float(Size.y), 1.0), bias);}\n"
                                                                                       "float vgpu_textureOffset(sampler2DShadow tex, vec3 P, vec2 offset){return vgpu_textureOffset(tex, P, offset, 0.0);}\n"
                                                                                       "vec4 vgpu_textureOffset(sampler2DArray tex, vec3 P, vec2 offset, float bias){ivec3 Size = textureSize(tex, 0);return texture(tex, P+vec3(offset.x, offset.y, 0)/vec3(float(Size.x), float(Size.y), float(Size.z)), bias);}\n"
                                                                                       "vec4 vgpu_textureOffset(sampler2DArray tex, vec3 P, vec2 offset){return vgpu_textureOffset(tex, P, offset, 0.0);}\n");

    source = WrapFunction(source, sourceLength, "shadow2D", "vgpu_shadow2D", "\nvec4 vgpu_shadow2D(sampler2DShadow shadow, vec3 coord){return vec4(texture(shadow, coord), 0.0, 0.0, 0.0);}\n"
                                                                              "vec4 vgpu_shadow2D(sampler2DShadow shadow, vec3 coord, float bias){return vec4(texture(shadow, coord, bias), 0.0, 0.0, 0.0);}\n");
    return source;
}

/**
 * Replace a function and its calls by a wrapper version, only if needed
 * @param source The shader code as a string
 * @param functionName The function to be replaced
 * @param wrapperFunctionName The replacing function name
 * @param function The wrapper function itself
 * @return The shader as a string, maybe in a different memory location
 */
char * WrapFunction(char * source, int * sourceLength, char * functionName, char * wrapperFunctionName, char * wrapperFunction){
    int originalSize = strlen(source);
    source = ReplaceFunctionName(source, sourceLength, functionName, wrapperFunctionName);
    // If some calls got replaced, add the wrapper
    if(originalSize != strlen(source)){
        int insertPoint = FindPositionAfterDirectives(source);
        source = InplaceInsertByIndex(source, sourceLength, insertPoint + 1, wrapperFunction);
    }

    return source;
}

/**
 * Replace the % operator with a mathematical equivalent (x - y * floor(x/y))
 * @param source The shader as a string
 * @return The shader as a string, maybe in a different memory location
 */
char * ReplaceModOperator(char * source, int * sourceLength) {
    char * modelString = " mod(x, y) ";
    int startIndex, endIndex = 0;
    int * startPtr = &startIndex, *endPtr = &endIndex;

    for(int i=0;i<*sourceLength; ++i){
        if(source[i] != '%') continue;
        // A mod operator is found !
        char * leftOperand = GetOperandFromOperator(source, i, 0, startPtr);
        char * rightOperand = GetOperandFromOperator(source,  i, 1, endPtr);

        // Generate a model string to be inserted
        char * replacementString = malloc(strlen(modelString) + 1);
        strcpy(replacementString, modelString);
        int replacementSize = strlen(replacementString);
        replacementString = InplaceReplace(replacementString, &replacementSize, "x", leftOperand);
        replacementString = InplaceReplace(replacementString, &replacementSize, "y", rightOperand);

        // Insert the new string
        source = InplaceReplaceByIndex(source, sourceLength, startIndex, endIndex, replacementString);

        // Free all the temporary strings
        free(leftOperand);
        free(rightOperand);
        free(replacementString);
    }

    return source;
}


/**
 * Change all (u)ints to floats.
 * This is a hack to avoid dealing with implicit conversions on common operators
 * @param source The shader as a string
 * @return The shader as a string, maybe in a new memory location
 * @see ForceIntegerArrayAccess
 */
char * CoerceIntToFloat(char * source, int * sourceLength){
    // Let's go the "freestyle way"

    // Step 1 is to translate keywords
    // Attempt and loop unrolling -> worked well, time to fix my shit I guess
    source = ReplaceVariableName(source, sourceLength, "int", "float");
    source = WrapFunction(source, sourceLength, "int", "float", "\n ");
    source = ReplaceVariableName(source, sourceLength, "uint", "float");
    source = WrapFunction(source, sourceLength, "uint", "float", "\n ");

    // TODO Yes I could just do the same as above but I'm lazy at times
    source = InplaceReplaceSimple(source, sourceLength, "ivec", "vec");
    source = InplaceReplaceSimple(source, sourceLength, "uvec", "vec");

    source = InplaceReplaceSimple(source, sourceLength, "isampleBuffer", "sampleBuffer");
    source = InplaceReplaceSimple(source, sourceLength, "usampleBuffer", "sampleBuffer");

    source = InplaceReplaceSimple(source, sourceLength, "isampler", "sampler");
    source = InplaceReplaceSimple(source, sourceLength, "usampler", "sampler");


    // Step 3 is slower.
    // We need to parse hardcoded values like 1 and turn it into 1.(0)
    for(int i=0; i<*sourceLength; ++i){

        // Avoid version/line directives
        if(source[i] == '#' && (source[i + 1] == 'v' || source[i + 1] == 'l') ){
            // Look for the next line
            while (source[i] != '\n' && source[i] != '\0'){
                i++;
            }
        }

        if(!isDigit(source[i])){ continue; }
        // So there is a few situations that we have to distinguish:
        // functionName1 (      ----- meaning there is SOMETHING on its left side that is related to the number
        // function(1,          ----- there is something, and it ISN'T related to the number
        // float test=3;        ----- something on both sides, not related to the number.
        // float test=X.2       ----- There is a dot, so it is part of a float already
        // float test = 0.00000 ----- I have to backtrack to find the dot

        if(source[i-1] == '.' || source[i+1] == '.') continue;// Number part of a float
        if(isValidFunctionName(source[i - 1])) continue; // Char attached to something related
        if(isDigit(source[i+1])) continue; // End of number not reached
        if(isDigit(source[i-1])){
            // Backtrack to check if the number is floating point
            int shouldBeCoerced = 0;
            for(int j=1; 1; ++j){
                if(isDigit(source[i-j])) continue;
                if(isValidFunctionName(source[i-j])) break; // Function or variable name, don't coerce
                if(source[i-j] == '.' || ((source[i-j] == '+' || source[i-j] == '-') && source[i-j-1] == 'e')) break; // No coercion, float or scientific notation already
                // Nothing found, should be coerced then
                shouldBeCoerced = 1;
                break;
            }

            if(!shouldBeCoerced) continue;
        }

        // Now we know there is nothing related to the digit, turn it into a float
        source = InplaceInsertByIndex(source, sourceLength, i+1, ".0");
    }

    // TODO Hacks for special built in values and typecasts ?
    source = InplaceReplaceSimple(source, sourceLength, "gl_VertexID", "float(gl_VertexID)");
    source = InplaceReplaceSimple(source, sourceLength, "gl_InstanceID", "float(gl_InstanceID)");

    return source;
}

/** Force all array accesses to use integers by adding an explicit typecast
 * @param source The shader as a string
 * @return The shader as a string, maybe at a new memory location */
char * ForceIntegerArrayAccess(char* source, int * sourceLength){
    char * markerStart = "$";
    char * markerEnd = "`";

    // Step 1, we need to mark all [] that are empty and must not be changed
    int leftCharIndex = 0;
    for(int i=0; i< *sourceLength; ++i){
        if(source[i] == '['){
            leftCharIndex = i;
            continue;
        }
        // If a start has been found
        if(leftCharIndex){
            if(source[i] == ' ' || source[i] == '\n'){
                continue;
            }
            // We find the other side and mark both ends
            if(source[i] == ']'){
                source[leftCharIndex] = *markerStart;
                source[i] = *markerEnd;
            }
        }
        //Something else is there, abort the marking phase for this one
        leftCharIndex = 0;
    }

    // Step 2, replace the array accesses with a forced typecast version
    source = InplaceReplaceSimple(source, sourceLength, "]", ")]");
    source = InplaceReplaceSimple(source, sourceLength, "[", "[int(");

    // Step 3, restore all marked empty []
    source = InplaceReplaceSimple(source, sourceLength, markerStart, "[");
    source = InplaceReplaceSimple(source, sourceLength, markerEnd, "]");

    return source;
}


/** Small helper to help evaluate whether to continue or not I guess
 * Values over 9900 are not for real operators, more like stop indicators*/
int GetOperatorValue(char operator){
    if(operator == ',' || operator == ';') return 9998;
    if(operator == '=') return 9997;
    if(operator == '+' || operator == '-') return 3;
    if(operator == '*' || operator == '/' || operator == '%') return 2;
    return NO_OPERATOR_VALUE; // Meaning no value;
}

/** Get the left or right operand, given the last index of the operator
 * It bases its ability to get operands by evaluating the priority of operators.
 * @param source The shader as a string
 * @param operatorIndex The index the operator is found
 * @param rightOperand Whether we get the right or left operator
 * @param limit The left or right index of the operand
 * @return newly allocated string with the operand
 */
char* GetOperandFromOperator(char* source, int operatorIndex, int rightOperand, int * limit){
    int parserState = 0;
    int parserDirection = rightOperand ? 1 : -1;
    int operandStartIndex = 0, operandEndIndex = 0;
    int parenthesesLeft = 0, hasFoundParentheses = 0;
    int operatorValue = GetOperatorValue(source[operatorIndex]);
    int lastOperator = 0; // Used to determine priority for unary operators

    char parenthesesStart = rightOperand ? '(' : ')';
    char parenthesesEnd = rightOperand ? ')' : '(';
    int stringIndex = operatorIndex;

    // Get to the operand
    while (parserState == 0){
        stringIndex += parserDirection;
        if(source[stringIndex] != ' '){
            parserState = 1;
            // Place the mark
            if(rightOperand){
                operandStartIndex = stringIndex;
            }else{
                operandEndIndex = stringIndex;
            }

            // Special case for unary operator when parsing to the right
            if(GetOperatorValue(source[stringIndex]) == 3 ){ // 3 is +- operators
                stringIndex += parserDirection;
            }
        }
    }

    // Get to the other side of the operand, the twist is here.
    while (parenthesesLeft > 0 || parserState == 1){

        // Look for parentheses
        if(source[stringIndex] == parenthesesStart){
            hasFoundParentheses = 1;
            parenthesesLeft += 1;
            stringIndex += parserDirection;
            continue;
        }

        if(source[stringIndex] == parenthesesEnd){
            hasFoundParentheses = 1;
            parenthesesLeft -= 1;

            // Likely to happen in a function call
            if(parenthesesLeft < 0){
                parserState = 3;
                if(rightOperand){
                    operandEndIndex = stringIndex - 1;
                }else{
                    operandStartIndex = stringIndex + 1;
                }
                continue;
            }
            stringIndex += parserDirection;
            continue;
        }

        // Small optimisation
        if(parenthesesLeft > 0){
            stringIndex += parserDirection;
            continue;
        }

        // So by now the following assumptions are made
        // 1 - We aren't between parentheses
        // 2 - No implicit multiplications are present
        // 3 - No fuckery with operators like "test = +-+-+-+-+-+-+-+-3;" although I attempt to support them

        // Higher value operators have less priority
        int currentValue = GetOperatorValue(source[stringIndex]);


        // The condition is different due to the evaluation order which is left to right, aside from the unary operators
        if((rightOperand ? currentValue >= operatorValue: currentValue > operatorValue)){
            if(currentValue == NO_OPERATOR_VALUE){
                if(source[stringIndex] == ' '){
                    stringIndex += parserDirection;
                    continue;
                }

                // Found an operand, so reset the operator eval for unary
                if(rightOperand) lastOperator = NO_OPERATOR_VALUE;

                // maybe it is the start of a function ?
                if(hasFoundParentheses){
                    parserState = 2;
                    continue;
                }
                // If no full () set is found, assume we didn't fully travel the operand
                stringIndex += parserDirection;
                continue;
            }

            // Special case when parsing unary operator to the right
            if(rightOperand && operatorValue == 3 && lastOperator < currentValue){
                stringIndex += parserDirection;
                continue;
            }

            // Stop, we found an operator of same worth.
            parserState = 3;
            if(rightOperand){
                operandEndIndex = stringIndex - 1;
            }else{
                operandStartIndex = stringIndex + 1;
            }
        }

        // Special case for unary operators from the right
        if(rightOperand && operatorValue == 3) { // 3 is + - operators
            lastOperator = currentValue;
        } // Special case for unary operators from the left
        if(!rightOperand && operatorValue < 3 && currentValue == 3){
            lastOperator = NO_OPERATOR_VALUE;
            for(int j=1; 1; ++j){
                int subCurrentValue = GetOperatorValue(source[stringIndex - j]);
                if(subCurrentValue != NO_OPERATOR_VALUE){
                    lastOperator = subCurrentValue;
                    continue;
                }

                // No operator value, can be almost anything
                if(source[stringIndex - j] == ' ') continue;
                // Else we found something. Did we found a high priority operator ?
                if(lastOperator <= operatorValue){ // If so, we allow continuing and going out of the loop
                    stringIndex -= j;
                    parserState = 1;
                    break;
                }
                // No other operator found
                operandStartIndex = stringIndex;
                parserState = 3;
                break;
            }
        }
        stringIndex += parserDirection;
    }

    // Status when we get the name of a function and nothing else.
    while (parserState == 2){
        if(source[stringIndex] != ' '){
            stringIndex += parserDirection;
            continue;
        }
        if(rightOperand){
            operandEndIndex = stringIndex - 1;
        }else{
            operandStartIndex = stringIndex + 1;
        }
        parserState = 3;
    }

    // At this point, we know both the start and end point of our operand, let's copy it
    char * operand = malloc(operandEndIndex - operandStartIndex + 2);
    memcpy(operand, source+operandStartIndex, operandEndIndex - operandStartIndex + 1);
    // Make sure the string is null terminated
    operand[operandEndIndex - operandStartIndex + 1] = '\0';

    // Send back the limitIndex
    *limit = rightOperand ? operandEndIndex : operandStartIndex;

    return operand;
}

/**
 * Replace any gl_FragData[n] reference by creating an out variable with the manual layout binding
 * @param source  The shader source as a string
 * @return The shader as a string, maybe at a different memory location
 */
char * ReplaceGLFragData(char * source, int * sourceLength){

    // 10 is arbitrary, but I don't expect the shader to use so many
    // TODO I guess the array could be accessed with one or more spaces :think:
    // TODO wait they can access via a variable !
    for (int i = 0; i < 10; ++i) {
        // Check for 2 forms on the glFragData and take the first one found
        char needle[30];
        sprintf(needle, "gl_FragData[%i]", i);

        // Skip if the draw buffer isn't used at this index
        char * useFragData = strstr(source, &needle[0]);
        if(useFragData == NULL){
            sprintf(needle, "gl_FragData[int(%i.0)]", i);
            useFragData = strstr(source, &needle[0]);
            if(useFragData == NULL) continue;
        }

        // Construct replacement string
        char replacement[20];
        char replacementLine[70];
        sprintf(replacement, "vgpu_FragData%i", i);
        sprintf(replacementLine, "\nlayout(location = %i) out mediump vec4 %s;\n", i, replacement);
        int insertPoint = FindPositionAfterDirectives(source);

        // And place them into the shader
        source = InplaceReplaceSimple(source, sourceLength, &needle[0], &replacement[0]);
        source = InplaceInsertByIndex(source, sourceLength, insertPoint + 1, &replacementLine[0]);
    }
    return source;
}

/**
 * Replace the gl_FragColor
 * @param source The shader as a string
 * @return The shader a a string, maybe in a different memory location
 */
char * ReplaceGLFragColor(char * source, int * sourceLength){
    if(strstr(source, "gl_FragColor")){
        source = InplaceReplaceSimple(source, sourceLength, "gl_FragColor", "vgpu_FragColor");
        int insertPoint = FindPositionAfterDirectives(source);
        source = InplaceInsertByIndex(source, sourceLength, insertPoint + 1, "out mediump vec4 vgpu_FragColor;\n");
    }
    return source;
}

/**
 * Remove all extensions right now by replacing them with spaces
 * @param source The shader as a string
 * @return The shader as a string, maybe in a different memory location
 */
char * RemoveUnsupportedExtensions(char * source){
    //TODO remove only specific extensions ?
    for(char * extensionPtr = strstr(source, "#extension "); extensionPtr; extensionPtr = strstr(source, "#extension ")){
        int i = 0;
        while(extensionPtr[i] != '\n'){
            extensionPtr[i] = ' ';
            ++i;
        }
    }
    return source;
}

/**
 * Replace the variable name in a shader, mostly used to avoid keyword clashing
 * @param source The shader as a string
 * @param initialName The initial name for the variable
 * @param newName The new name for the variable
 * @return The shader as a string, maybe in a different memory location
 */
char * ReplaceVariableName(char * source, int * sourceLength, char * initialName, char* newName) {

    char * toReplace = malloc(strlen(initialName) + 3);
    char * replacement = malloc(strlen(newName) + 3);
    char * charBefore = "{}([];+-*/~!%<>,&| \n\t";
    char * charAfter = ")[];+-*/%<>;,|&. \n\t";

    // Prepare the fixed part of the strings
    strcpy(toReplace+1, initialName);
    toReplace[strlen(initialName)+2] = '\0';

    strcpy(replacement+1, newName);
    replacement[strlen(newName)+2] = '\0';

    for (int i = 0; i < strlen(charBefore); ++i) {
        for (int j = 0; j < strlen(charAfter); ++j) {
            // Prepare the string to replace
            toReplace[0] = charBefore[i];
            toReplace[strlen(initialName)+1] = charAfter[j];

            // Prepare the replacement string
            replacement[0] = charBefore[i];
            replacement[strlen(newName)+1] = charAfter[j];

            source = InplaceReplaceSimple(source, sourceLength, toReplace, replacement);
        }
    }

    free(toReplace);
    free(replacement);

    return source;
}

/**
 * Replace a function definition and calls to the function to another name
 * @param source The shader as a string
 * @param sourceLength The shader length
 * @param initialName The name be to changed
 * @param finalName The name to use instead
 * @return The shader as a string, maybe in a different memory location
 */
char * ReplaceFunctionName(char * source, int * sourceLength, char * initialName, char * finalName){
    for(unsigned long currentPosition = 0; 1; currentPosition += strlen(initialName)){
        unsigned long newPosition = strstrPos(source + currentPosition, initialName);
        if(newPosition == 0) // No more calls
            break;

        // Check if that is indeed a function call on the right side
        if (source[GetNextTokenPosition(source, currentPosition + newPosition + strlen(initialName), '(', " \n\t")] != '('){
            currentPosition += newPosition;
            continue; // Skip to the next potential call
        }

        // Check the naming on the left side
        if (isValidFunctionName(source[currentPosition + newPosition - 1])){
            currentPosition += newPosition;
            continue; // Skip to the next potential call
        }

        // This is a valid function call/definition, replace it
        source = InplaceReplaceByIndex(source, sourceLength, currentPosition + newPosition, currentPosition + newPosition + strlen(initialName) - 1, finalName);
        currentPosition += newPosition;
    }
    return source;
}

char * ConvertShaderConditionally(struct shader_s * shader_source){
    int shaderCompileStatus;

    // First, vanilla gl4es, no forward port
    shader_source->converted = ConvertShader(shader_source->source, shader_source->type == GL_VERTEX_SHADER ? 1 : 0,&shader_source->need, 0);
    shaderCompileStatus = testGenericShader(shader_source);

    // Then, attempt back porting if desired of constrained to do so
    if(!shaderCompileStatus && globals4es.vgpu_backport) {
        shader_source->converted = ConvertShader(shader_source->source, shader_source->type == GL_VERTEX_SHADER ? 1 : 0,&shader_source->need, 0);
        shader_source->converted = ConvertShaderVgpu(shader_source);
        shaderCompileStatus = testGenericShader(shader_source);
    }

    // At last resort, use forward porting
    if(!shaderCompileStatus && hardext.glsl300es){
        shader_source->converted = ConvertShader(shader_source->source, shader_source->type == GL_VERTEX_SHADER ? 1 : 0, &shader_source->need, 1);
        shader_source->converted = ConvertShaderVgpu(shader_source);
    }

    // Process uniform declarations
    shader_source->converted = process_uniform_declarations(shader_source->converted, shader_source->uniforms_declarations, &shader_source->uniforms_declarations_count);
    return shader_source->converted;
}

/** Convert the shader through multiple steps
 * @param source The start of the shader as a string*/
char * ConvertShaderVgpu(struct shader_s * shader_source){

    if (globals4es.vgpu_dump){
        SHUT_LOGD("New VGPU Shader source:\n%s\n", shader_source->converted);
    }

    // Get the shader source
    char * source = shader_source->converted;
    int sourceLength = strlen(source) + 1;
    // For now, skip stuff
    if(FindString(source, "#version 100")){
        if(globals4es.vgpu_force_conv || globals4es.vgpu_backport){
            if (shader_source->type == GL_VERTEX_SHADER){
                source = ReplaceVariableName(source, &sourceLength, "in", "attribute");
                source = ReplaceVariableName(source, &sourceLength, "out", "varying");
            }else{
                source = ReplaceVariableName(source, &sourceLength, "in", "varying");
                source = ReplaceFragmentOut(source, &sourceLength);
            }

            // Well, we don't have gl_VertexID on OPENGL 1
            source = ReplaceVariableName(source, &sourceLength, "gl_VertexID", "0");
            source = InplaceReplaceSimple(source, &sourceLength, "ivec", "vec");
            source = InplaceReplaceSimple(source, &sourceLength, "bvec", "vec");
            source = InplaceReplaceSimple(source, &sourceLength, "flat ", "");

            source = BackportConstArrays(source, &sourceLength);
            int insertPoint = FindPositionAfterVersion(source);
            source = InplaceInsertByIndex(source, &sourceLength, insertPoint + 1, "#define texelFetch(a, b, c) vec4(1.0,1.0,1.0,1.0) \n");

            source = ReplaceModOperator(source, &sourceLength);

            if (globals4es.vgpu_dump){
                SHUT_LOGD("New VGPU Shader conversion:\n%s\n", source);
            }

            return source;
        }

        // Else, skip the conversion
        if (globals4es.vgpu_dump){
            SHUT_LOGD("SKIPPING OLD SHADER CONVERSION \n%s\n", source);
        }
        return source;
    }


    // Remove 'const' storage qualifier
    //printf("REMOVING CONST qualifiers");
    //source = RemoveConstInsideBlocks(source, &sourceLength);
    //source = ReplaceVariableName(source, &sourceLength, "const", " ");




    // Avoid keyword clash with gl4es #define blocks
    //printf("REPLACING KEYWORDS");
    source = InplaceReplaceSimple(source, &sourceLength, "#define texture2D texture\n", "");
    source = ReplaceVariableName(source, &sourceLength, "sample", "vgpu_Sample");
    source = ReplaceVariableName(source, &sourceLength, "texture", "vgpu_texture");

    source = ReplaceFunctionName(source, &sourceLength, "texture2D", "texture");
    source = ReplaceFunctionName(source, &sourceLength, "texture3D", "texture");
    source = ReplaceFunctionName(source, &sourceLength, "texture2DLod", "textureLod");


    //printf("REMOVING \" CHARS ");
    // " not really supported here
    source = InplaceReplaceSimple(source, &sourceLength, "\"", "");

    // For now let's hope no extensions are used
    // TODO deal with extensions but properly
    //printf("REMOVING EXTENSIONS");
    //source = RemoveUnsupportedExtensions(source);

    // OpenGL natively supports non const global initializers, not OPENGL ES except if we add an extension
    //printf("ADDING EXTENSIONS\n");
    source = InsertExtensions(source, &sourceLength);

    //printf("REPLACING mod OPERATORS");
    // No support for % operator, so we replace it
    source = ReplaceModOperator(source, &sourceLength);

    //printf("COERCING INT TO FLOATS");
    // Hey we don't want to deal with implicit type stuff
    source = CoerceIntToFloat(source, &sourceLength);

    //printf("FIXING ARRAY ACCESS");
    // Avoid any weird type trying to be an index for an array
    source = ForceIntegerArrayAccess(source, &sourceLength);

    //printf("WRAPPING FUNCTION");
    // Since everything is a float, we need to overload WAY TOO MANY functions
    source = WrapIvecFunctions(source, &sourceLength);

    //printf("REMOVING DUBIOUS DEFINES");
    source = InplaceReplaceSimple(source, &sourceLength, "#define texture texture2D\n", "");
    source = InplaceReplaceSimple(source, &sourceLength, "#define attribute in\n", "");
    source = InplaceReplaceSimple(source, &sourceLength, "#define varying out\n", "");

    if (shader_source->type == GL_VERTEX_SHADER){
        source = ReplaceVariableName(source, &sourceLength, "attribute", "in");
        source = ReplaceVariableName(source, &sourceLength, "varying", "out");
    }else{
        source = ReplaceVariableName(source, &sourceLength, "varying", "in");
    }

    // Draw buffers aren't dealt the same on OPEN GL|ES
    if(shader_source->type == GL_FRAGMENT_SHADER && doesShaderVersionContainsES(source) ){
        //printf("REPLACING FRAG DATA");
        source = ReplaceGLFragData(source, &sourceLength);
        //printf("REPLACING FRAG COLOR");
        source = ReplaceGLFragColor(source, &sourceLength);
    }

    //printf("FUCKING UP PRECISION");
    source = ReplacePrecisionQualifiers(source, &sourceLength, shader_source->type == GL_VERTEX_SHADER);
    
    source = ProcessSwitchCases(source, &sourceLength);

    if (globals4es.vgpu_dump){
        SHUT_LOGD("New VGPU Shader conversion:\n%s\n", source);
    }

    return source;
}

char* ConvertShader(const char* pEntry, int isVertex, shaderconv_need_t *need, int forwardPort)
{
    if(gl_VA[0][0]=='\0') {
    for (int i=0; i<MAX_VATTRIB; ++i) {
    sprintf(gl_VA[i], "%s%d", gl_VertexAttrib, i);
    sprintf(gl4es_VA[i], "%s%d", gl4es_VertexAttrib, i);
    }
    }
    int fpeShader = (strstr(pEntry, fpeshader_signature)!=NULL)?1:0;
    int maskbefore = 4|(isVertex?1:2);
    int maskafter = 8|(isVertex?1:2);
    if((globals4es.dbgshaderconv&maskbefore)==maskbefore) {
    printf("Shader source%s:\n%s\n", pEntry, fpeShader?" (FPEShader generated)":"");
    }
    int comments = globals4es.comments;
    
    char* pBuffer = (char*)pEntry;
    
    int version120 = 0;
    char* versionString = NULL;
    if(!fpeShader) {
    extensions_t exts;  // dummy...
    exts.cap = exts.size = 0;
    exts.ext = NULL;
    // hacks
    char* pHacked = ShaderHacks(pBuffer);
    // preproc first
    pBuffer = preproc(pHacked, comments, globals4es.shadernogles, &exts, &versionString);
    if(pHacked!=pEntry && pHacked!=pBuffer)
    free(pHacked);
    // now comment all line starting with precision...
    if(strstr(pBuffer, "\nprecision")) {
    int sz = strlen(pBuffer);
    pBuffer = InplaceReplace(pBuffer, &sz, "\nprecision", "\n//precision");
    }
    // should do something with the extension list...
    if(exts.ext)
    free(exts.ext);
    }
    
    static shaderconv_need_t dummy_need = {0};
    if(!need) {
    need = &dummy_need;
    need->need_texcoord = -1;
    need->need_clean = 1; // no hack, this is a dummy need structure
    }
    int notexarray = globals4es.notexarray || need->need_notexarray || fpeShader;
    
    //const char* GLESUseFragHighp = "#extension GL_OES_fragment_precision_high : enable\n"; // does this one is needed?  
    char GLESFullHeader[512];
    int wanthighp = !fpeShader;
    if(wanthighp && !hardext.highp) wanthighp = 0;
    int versionHeader = 0;
    if(versionString && strcmp(versionString, "120")==0)
    version120 = 1;
    //if(version120) {
    /*if(hardext.glsl300es) {
    versionHeader = 1;
    if(hardext.glsl310es) {
    versionHeader = 2;
    if(hardext.glsl320es) {
    	versionHeader = 3;
    }
    }
    }*/
    /* location on uniform not supported ! */ 
    /* else no location or in / out are supported */
    //}
    
    //sprintf(GLESFullHeader, GLESHeader[versionHeader], "");
    // pack/shaderconv.h shaderconv.c: old_version new_version
    sprintf(GLESFullHeader, old_version, "");
    sprintf(GLESFullHeader+strlen(old_version), "\n\n");
    
    
    int tmpsize = strlen(pBuffer)*2+strlen(GLESFullHeader)+100;
    char* Tmp = (char*)calloc(1, tmpsize);
    strcpy(Tmp, pBuffer);
    
    // and now change the version header, and add default precision
    char* newptr;
    newptr=strstr(Tmp, "#version");
    if (!newptr) {
    Tmp = InplaceInsert(Tmp, GLESFullHeader, Tmp, &tmpsize);
    } else {
    while(*newptr!=0x0a) newptr++;
    newptr++;
    memmove(Tmp, newptr, strlen(newptr)+1);
    Tmp = InplaceInsert(Tmp, GLESFullHeader, Tmp, &tmpsize);
    }
    int headline = 3;
    // check if gl_FragDepth is used
    int fragdepth = (strstr(pBuffer, "gl_FragDepth"))?1:0;
    const char* GLESUseFragDepth = "//#extension GL_EXT_frag_depth : enable\n";
    const char* GLESFakeFragDepth = "mediump float fakeFragDepth = 0.0;\n";
    if (fragdepth) {
    /* If #extension is used, it should be placed before the second line of the header. */
    if(hardext.fragdepth)
    Tmp = InplaceInsert(GetLine(Tmp, 1), GLESUseFragDepth, Tmp, &tmpsize);
    else
    Tmp = InplaceInsert(GetLine(Tmp, headline-1), GLESFakeFragDepth, Tmp, &tmpsize);
    headline++;
    }
    int derivatives = (strstr(pBuffer, "dFdx(") || strstr(pBuffer, "dFdy(") || strstr(pBuffer, "fwidth("))?1:0;
    const char* GLESUseDerivative = "#extension GL_OES_standard_derivatives : enable\n";
    // complete fake value... A better thing should be use....
    const char* GLESFakeDerivative = "float dFdx(float p) {return 0.0001;}\nvec2 dFdx(vec2 p) {return vec2(0.0001);}\nvec3 dFdx(vec3 p) {return vec3(0.0001);}\n"
    "float dFdy(float p) {return 0.0001;}\nvec2 dFdy(vec2 p) {return vec2(0.0001);}\nvec3 dFdy(vec3 p) {return vec3(0.0001);}\n"
    "float fwidth(float p) {return abs(dFdx(p))+abs(dFdy(p));}\nvec2 fwidth(vec2 p) {return abs(dFdx(p))+abs(dFdy(p));}\n"
    "vec3 fwidth(vec3 p) {return abs(dFdx(p))+abs(dFdy(p));}\n";
    if (derivatives) {
    /* If #extension is used, it should be placed before the second line of the header. */
    if(hardext.derivatives)
    Tmp = InplaceInsert(GetLine(Tmp, 1), GLESUseDerivative, Tmp, &tmpsize);
    else
    Tmp = InplaceInsert(GetLine(Tmp, headline-1), GLESFakeDerivative, Tmp, &tmpsize);
    headline++;
    }
    
    LOAD_GLES_(glGetIntegerv);
    gles_glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS_EXT, &hardext.maxcolorattach);
    gles_glGetIntegerv(GL_MAX_DRAW_BUFFERS_ARB, &hardext.maxdrawbuffers);
    
    // check if draw_buffers may be used (no fallback here :( )
    /*if(hardext.maxdrawbuffers>1 && strstr(pBuffer, "gl_FragData")) {
    Tmp = InplaceInsert(GetLine(Tmp, 1), useEXTDrawBuffers, Tmp, &tmpsize);
    }*/
    
    
    
    // if some functions are used, add some int/float alternative
    /*
    if(!fpeShader && !globals4es.nointovlhack) {
    if(strstr(Tmp, "pow(") || strstr(Tmp, "pow (")) {
    Tmp = InplaceInsert(GetLine(Tmp, headline), HackAltPow, Tmp, &tmpsize);
    }
    if(strstr(Tmp, "max(") || strstr(Tmp, "max (")) {
    Tmp = InplaceInsert(GetLine(Tmp, headline), HackAltMax, Tmp, &tmpsize);
    }
    if(strstr(Tmp, "min(") || strstr(Tmp, "min (")) {
    Tmp = InplaceInsert(GetLine(Tmp, headline), HackAltMin, Tmp, &tmpsize);
    }
    if(strstr(Tmp, "clamp(") || strstr(Tmp, "clamp (")) {
    Tmp = InplaceInsert(GetLine(Tmp, headline), HackAltClamp, Tmp, &tmpsize);
    }
    if(strstr(Tmp, "mod(") || strstr(Tmp, "mod (")) {
    Tmp = InplaceInsert(GetLine(Tmp, headline), HackAltMod, Tmp, &tmpsize);
    }
    }
    */
    
    
    /*
    if(!isVertex && hardext.shaderlod && 
    (FindString(Tmp, "texture2DLod") || FindString(Tmp, "texture2DProjLod") 
    || FindString(Tmp, "textureCubeLod") 
    || FindString(Tmp, "texture2DGradARB") || FindString(Tmp, "texture2DProjGradARB")|| FindString(Tmp, "textureCubeGradARB") 
    )) {
    const char* GLESUseShaderLod = "#extension GL_EXT_shader_texture_lod : enable\n";
    Tmp = InplaceInsert(GetLine(Tmp, 1), GLESUseShaderLod, Tmp, &tmpsize);
    }
    */
    
    /*
    if(!isVertex && (FindString(Tmp, "texture2DLod"))) {
    if(hardext.shaderlod) {
    Tmp = InplaceReplace(Tmp, &tmpsize, "texture2DLod", "texture2DLodEXT");
    } else {
    Tmp = InplaceReplace(Tmp, &tmpsize, "texture2DLod", "_gl4es_texture2DLod");
    Tmp = InplaceInsert(GetLine(Tmp, headline), texture2DLodAlt, Tmp, &tmpsize);
    }
    }
    if(!isVertex && (FindString(Tmp, "texture2DProjLod"))) {
    if(hardext.shaderlod) {
    Tmp = InplaceReplace(Tmp, &tmpsize, "texture2DProjLod", "texture2DProjLodEXT");
    } else {
    Tmp = InplaceReplace(Tmp, &tmpsize, "texture2DProjLod", "_gl4es_texture2DProjLod");
    Tmp = InplaceInsert(GetLine(Tmp, headline), texture2DProjLodAlt, Tmp, &tmpsize);
    }
    }
    if(!isVertex && (FindString(Tmp, "textureCubeLod"))) {
    if(hardext.shaderlod) {
    if(!hardext.cubelod)
      Tmp = InplaceReplace(Tmp, &tmpsize, "textureCubeLod", "textureCubeLodEXT");
    } else {
    Tmp = InplaceReplace(Tmp, &tmpsize, "textureCubeLod", "_gl4es_textureCubeLod");
    Tmp = InplaceInsert(GetLine(Tmp, headline), textureCubeLodAlt, Tmp, &tmpsize);
    }
    }
    if(!isVertex && (FindString(Tmp, "texture2DGradARB"))) {
    if(hardext.shaderlod) {
    Tmp = InplaceReplace(Tmp, &tmpsize, "texture2DGradARB", "texture2DGradEXT");
    } else {
    Tmp = InplaceReplace(Tmp, &tmpsize, "texture2DGradARB", "_gl4es_texture2DGrad");
    Tmp = InplaceInsert(GetLine(Tmp, headline), texture2DGradAlt, Tmp, &tmpsize);
    }
    }
    if(!isVertex && (FindString(Tmp, "texture2DProjGradARB"))) {
    if(hardext.shaderlod) {
    Tmp = InplaceReplace(Tmp, &tmpsize, "texture2DProjGradARB", "texture2DProjGradEXT");
    } else {
    Tmp = InplaceReplace(Tmp, &tmpsize, "texture2DProjGradARB", "_gl4es_texture2DProjGrad");
    Tmp = InplaceInsert(GetLine(Tmp, headline), texture2DProjGradAlt, Tmp, &tmpsize);
    }
    }
    if(!isVertex && (FindString(Tmp, "textureCubeGradARB"))) {
    if(hardext.shaderlod) {
    if(!hardext.cubelod)
      Tmp = InplaceReplace(Tmp, &tmpsize, "textureCubeGradARB", "textureCubeGradEXT");
    } else {
    Tmp = InplaceReplace(Tmp, &tmpsize, "textureCubeGradARB", "_gl4es_textureCubeGrad");
    Tmp = InplaceInsert(GetLine(Tmp, headline), textureCubeGradAlt, Tmp, &tmpsize);
    }
    }
    */
    
    // now check to remove trailling "f" after float, as it's not supported too
    newptr = Tmp;
    // simple state machine...
    /*
    int state = 0;
    while (*newptr!=0x00) {
    switch(state) {
    case 0:
    if ((*newptr >= '0') && (*newptr <= '9'))
      state = 1;  // integer part
    else if (*newptr == '.')
      state = 2;  // fractional part
    else if ((*newptr==' ') || (*newptr==0x0d) || (*newptr==0x0a) || (*newptr=='-') || (*newptr=='+') || (*newptr=='*') || (*newptr=='/') || (*newptr=='(') || (*newptr==')' || (*newptr=='>') || (*newptr=='<')))
      state = 0; // separator
    else 
      state = 3; // something else
    break;
    case 1: // integer part
    if ((*newptr >= '0') && (*newptr <= '9'))
      state = 1;  // integer part
    else if (*newptr == '.')
      state = 2;  // fractional part
    else if ((*newptr==' ') || (*newptr==0x0d) || (*newptr==0x0a) || (*newptr=='-') || (*newptr=='+') || (*newptr=='*') || (*newptr=='/') || (*newptr=='(') || (*newptr==')' || (*newptr=='>') || (*newptr=='<')))
      state = 0; // separator
    else  if (*newptr == 'f' ) {
      // remove that f
      memmove(newptr, newptr+1, strlen(newptr+1)+1);
      newptr--;
    } else
      state = 3;
      break;
    case 2: // fractionnal part
    if ((*newptr >= '0') && (*newptr <= '9'))
      state = 2;
    else if ((*newptr==' ') || (*newptr==0x0d) || (*newptr==0x0a) || (*newptr=='-') || (*newptr=='+') || (*newptr=='*') || (*newptr=='/') || (*newptr=='(') || (*newptr==')' || (*newptr=='>') || (*newptr=='<')))
      state = 0; // separator
    else  if (*newptr == 'f' ) {
      // remove that f
      memmove(newptr, newptr+1, strlen(newptr+1)+1);
      newptr--;
    } else
      state = 3;
      break;
    case 3:
    if ((*newptr==' ') || (*newptr==0x0d) || (*newptr==0x0a) || (*newptr=='-') || (*newptr=='+') || (*newptr=='*') || (*newptr=='/') || (*newptr=='(') || (*newptr==')' || (*newptr=='>') || (*newptr=='<')))
      state = 0; // separator
    else      
      state = 3;
      break;
    }
    newptr++;
    }
    */
    
    //Tmp = InplaceReplace(Tmp, &tmpsize, "gl_FragDepth", (hardext.fragdepth)?"gl_FragDepthEXT":"fakeFragDepth");
    // builtin attribs
    if(isVertex) {
    // check for ftransform function
    if(strstr(Tmp, "ftransform(")) {
    Tmp = InplaceInsert(GetLine(Tmp, headline), gl4es_ftransformSource, Tmp, &tmpsize);
    // don't increment headline count, as all variying and attributes should be created before
    }
    // check for builtin OpenGL attributes...
    int n = sizeof(builtin_attrib)/sizeof(builtin_attrib_t);
    for (int i=0; i<n; i++) {
      if(strstr(Tmp, builtin_attrib[i].glname)) {
          // ok, this attribute is used
          // replace gl_name by _gl4es_ one
          Tmp = InplaceReplace(Tmp, &tmpsize, builtin_attrib[i].glname, builtin_attrib[i].name);
          // insert a declaration of it
          char def[100];
          sprintf(def, "attribute %s %s %s;\n", builtin_attrib[i].prec, builtin_attrib[i].type, builtin_attrib[i].name);
          Tmp = InplaceInsert(GetLine(Tmp, headline++), def, Tmp, &tmpsize);
      }
    }
    if(strstr(Tmp, gl_VertexAttrib)) {
    // Generic VA from Old Programs
    for (int i=0; i<MAX_VATTRIB; ++i) {
      char A[100];
      if(FindString(Tmp, gl_VA[i])) {
        sprintf(A, "attribute highp vec4 %s%d;\n", gl4es_VertexAttrib, i);
        Tmp = InplaceReplace(Tmp, &tmpsize, gl_VA[i], gl4es_VA[i]);
        Tmp = InplaceInsert(GetLine(Tmp, headline++), A, Tmp, &tmpsize);
      }
    }
    }
    }
    // builtin varying
    int nvarying = 0;
    if(strstr(Tmp, "gl_Color") || need->need_color) {
    if(need->need_color<1) need->need_color = 1;
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_Color", (need->need_color==1)?"gl_FrontColor":"(gl_FrontFacing?gl_FrontColor:gl_BackColor)");
    }
    if(strstr(Tmp, "gl_FrontColor") || need->need_color) {
    if(need->need_color<1) need->need_color = 1;
    nvarying+=1;
    Tmp = InplaceInsert(GetLine(Tmp, headline), gl4es_frontColorSource, Tmp, &tmpsize);
    headline+=CountLine(gl4es_frontColorSource);
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_FrontColor", "_gl4es_FrontColor");
    }
    if(strstr(Tmp, "gl_BackColor") || (need->need_color==2)) {
    need->need_color = 2;
    nvarying+=1;
    Tmp = InplaceInsert(GetLine(Tmp, headline), gl4es_backColorSource, Tmp, &tmpsize);
    headline+=CountLine(gl4es_backColorSource);
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_BackColor", "_gl4es_BackColor");
    }
    if(strstr(Tmp, "gl_SecondaryColor") || need->need_secondary) {
    if(need->need_secondary<1) need->need_secondary = 1;
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_SecondaryColor", (need->need_secondary==1)?"gl_FrontSecondaryColor":"(gl_FrontFacing?gl_FrontSecondaryColor:gl_BackSecondaryColor)");
    }
    if(strstr(Tmp, "gl_FrontSecondaryColor") || need->need_secondary) {
    if(need->need_secondary<1) need->need_secondary = 1;
    nvarying+=1;
    Tmp = InplaceInsert(GetLine(Tmp, headline), gl4es_frontSecondaryColorSource, Tmp, &tmpsize);
    headline+=CountLine(gl4es_frontSecondaryColorSource);
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_FrontSecondaryColor", "_gl4es_FrontSecondaryColor");
    }
    if(strstr(Tmp, "gl_BackSecondaryColor") || (need->need_secondary==2)) {
    need->need_secondary = 2;
    nvarying+=1;
    Tmp = InplaceInsert(GetLine(Tmp, headline), gl4es_backSecondaryColorSource, Tmp, &tmpsize);
    headline+=CountLine(gl4es_backSecondaryColorSource);
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_BackSecondaryColor", "_gl4es_BackSecondaryColor");
    }
    if(strstr(Tmp, "gl_FogFragCoord") || need->need_fogcoord) {
    need->need_fogcoord = 1;
    nvarying+=1;
    Tmp = InplaceInsert(GetLine(Tmp, headline), gl4es_fogcoordSource, Tmp, &tmpsize);
    headline+=CountLine(gl4es_fogcoordSource);
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_FogFragCoord", "_gl4es_FogFragCoord");
    }
    // Get the max_texunit and the calc notexarray
    if(strstr(Tmp, "gl_TexCoord") || need->need_texcoord!=-1) {
    int ntex = need->need_texcoord;
    // Try to determine max gl_TexCoord used
    char* p = Tmp;
    int notexarray_ok = 1;
    while((p=strstr(p, gl_TexCoordSource))) {
    p+=strlen(gl_TexCoordSource);
    if(*p>='0' && *p<='9') {
    int n = (*p) - '0';
    if(p[1]>='0' && p[1]<='9')
      n = n*10 + (p[1] - '0');
    if (ntex<n) ntex = n;
    } else 
    notexarray_ok=0;
    }
    // if failed to determine, take max...
    if (ntex==-1) ntex = hardext.maxtex;
    // check constraint, and switch to notexarray if needed
    if (!notexarray && ntex+nvarying>hardext.maxvarying && !need->need_clean && notexarray_ok) {
    notexarray = 1;
    need->need_notexarray = 1;
    }
    // prefer notexarray...
    if(!isVertex && notexarray_ok && !need->need_clean) {
    notexarray = 1;
    need->need_notexarray = 1;
    }
    // check constaints
    if (!notexarray && ntex+nvarying>hardext.maxvarying) ntex = hardext.maxvarying - nvarying;
    need->need_texcoord = ntex;
    char d[100];
    if(notexarray) {
    for (int k=0; k<ntex+1; k++) {
    char d2[100];
    sprintf(d2, "gl_TexCoord[%d]", k);
    if(strstr(Tmp, d2)) {
      sprintf(d, gl4es_texcoordSourceAlt, k);
      Tmp = InplaceInsert(GetLine(Tmp, headline), d, Tmp, &tmpsize);
      headline+=CountLine(d);
      sprintf(d, "_gl4es_TexCoord_%d", k);
      Tmp = InplaceReplace(Tmp, &tmpsize, d2, d);
    }
    // check if texture is there
    sprintf(d2, "_gl4es_TexCoord_%d", k);
    if(strstr(Tmp, d2))
      need->need_texs |= (1<<k);
    }
    } else {
    sprintf(d, gl4es_texcoordSource, ntex+1);
    Tmp = InplaceInsert(GetLine(Tmp, headline), d, Tmp, &tmpsize);
    headline+=CountLine(d);
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_TexCoord", "_gl4es_TexCoord");
    // set textures as all ntex used
    for (int k=0; k<ntex+1; k++)
    need->need_texs |= (1<<k);
    }
    }
    
    // builtin matrices work
    {
    if(strstr(Tmp, "transpose(") || strstr(Tmp, "transpose ") || strstr(Tmp, "transpose\t")) {
    Tmp = InplaceInsert(GetLine(Tmp, headline), gl4es_transpose, Tmp, &tmpsize);
    InplaceReplace(Tmp, &tmpsize, "transpose", "gl4es_transpose");
    // don't increment headline count, as all variying and attributes should be created before
    }
    // check for builtin matrix uniform...
    {
    // first check number of texture matrices used
    int ntex = -1;
    // Try to determine max Texture matrice used, for each transposed inverse or regular...
    for(int i=0; i<4; ++i) {
    char* p = Tmp;
    while((p=strstr(p, gl_TexMatrixSources[i]))) {
      p+=strlen(gl_TexMatrixSources[i]);
      if(*p>='0' && *p<='9') {
        int n = 0;
        while(*p>='0' && *p<='9')
          n = n*10 + (*(p++) - '0');
        
        if (ntex<n) ntex = n;
      }
    }
    }
    
    // if failed to determine, take max...
    if (ntex==-1) ntex = need->need_texcoord; else ++ntex;
    // change gl_TextureMatrix[X] to gl_TextureMatrix_X if possible
    int change_textmat = notexarray;
    if(!change_textmat) {
    change_textmat = 1;
    char* p = Tmp;
    while(change_textmat && (p=strstr(p, "gl_TextureMatrix["))) {
      p += strlen("gl_TextureMatrix[");
      while((*p)>='0' && (*p)<='9') ++p;
      if((*p)!=']')
        change_textmat = 0;
    }
    }
    if(change_textmat) {
    for (int k=0; k<ntex+1; k++) {
      char d[100];
      char d2[100];
      sprintf(d2, "gl_TextureMatrix[%d]", k);
      if(strstr(Tmp, d2)) {
        sprintf(d, "gl_TextureMatrix_%d", k);
        Tmp = InplaceReplace(Tmp, &tmpsize, d2, d);
      }
    }
    }
    
    int n = sizeof(builtin_matrix)/sizeof(builtin_matrix_t);
    for (int i=0; i<n; i++) {
      if(strstr(Tmp, builtin_matrix[i].glname)) {
          // ok, this matrix is used
          // replace gl_name by _gl4es_ one
          Tmp = InplaceReplace(Tmp, &tmpsize, builtin_matrix[i].glname, builtin_matrix[i].name);
          // insert a declaration of it
          char def[100];
          int ishighp = (isVertex || hardext.highp)?1:0;
          if(builtin_matrix[i].matrix == MAT_N) {
            if(need->need_normalmatrix && !hardext.highp)
              ishighp = 0;
            if(!hardext.highp && !isVertex)
              need->need_normalmatrix = 1;
          }
          if(builtin_matrix[i].matrix == MAT_MV) {
            if(need->need_mvmatrix && !hardext.highp)
              ishighp = 0;
            if(!hardext.highp && !isVertex)
              need->need_mvmatrix = 1;
          }
          if(builtin_matrix[i].matrix == MAT_MVP) {
            if(need->need_mvpmatrix && !hardext.highp)
              ishighp = 0;
            if(!hardext.highp && !isVertex)
              need->need_mvpmatrix = 1;
          }
          if(builtin_matrix[i].texarray)
              sprintf(def, "uniform %s%s %s[%d];\n", (ishighp)?"highp ":"mediump ", builtin_matrix[i].type, builtin_matrix[i].name, ntex);
          else
              sprintf(def, "uniform %s%s %s;\n", (ishighp)?"highp ":"mediump ", builtin_matrix[i].type, builtin_matrix[i].name);
          Tmp = InplaceInsert(GetLine(Tmp, headline++), def, Tmp, &tmpsize);
      }
    }
    }
    }
    // Handling of gl_LightSource[x].halfVector => normalize(gl_LightSource[x].position - gl_Vertex), but what if in the FragShader ?
    /*  if(strstr(Tmp, "halfVector"))
    {
    char *p = Tmp;
    while((p=strstr(p, "gl_LightSource["))) {
    char *p2 = strchr(p, ']');
    if (p2 && !strncmp(p2, "].halfVector", strlen("].halfVector"))) {
    // found an occurence, lets change
    char p3[500];
    strncpy(p3,p, (p2-p)+1); p3[(p2-p)+1]='\0';
    char p4[500], p5[500];
    sprintf(p4, "%s.halfVector", p3);
    sprintf(p5, "normalize(normalize(%s.position.xyz) + vec3(0., 0., 1.))", p3);
    Tmp = InplaceReplace(Tmp, &tmpsize, p4, p5);
    p = Tmp;
    } else
    ++p;
    }
    }*/
    // cleaning up the "centroid" keyword...
    if(strstr(Tmp, "centroid"))
    {
    char *p = Tmp;
    while((p=strstr(p, "centroid"))!=NULL)
    {
    if(p[8]==' ' || p[8]=='\t') { // what next...
    const char* p2 = GetNextStr(p+8);
    if(strcmp(p2, "uniform")==0 || strcmp(p2, "varying")==0) {
      memset(p, ' ', 8);  // erase the keyword...
    }
    } 
    p+=8;
    }
    }
    
    // check for builtin OpenGL gl_LightSource & friends
    if(strstr(Tmp, "gl_LightSourceParameters") || strstr(Tmp, "gl_LightSource"))
    {
    Tmp = InplaceInsert(GetLine(Tmp, headline), gl4es_LightSourceParametersSource, Tmp, &tmpsize);
    headline+=CountLine(gl4es_LightSourceParametersSource);
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_LightSourceParameters", "_gl4es_LightSourceParameters");
    }
    if(strstr(Tmp, "gl_LightModelParameters") || strstr(Tmp, "gl_LightModel"))
    {
    Tmp = InplaceInsert(GetLine(Tmp, headline), gl4es_LightModelParametersSource, Tmp, &tmpsize);
    headline+=CountLine(gl4es_LightModelParametersSource);
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_LightModelParameters", "_gl4es_LightModelParameters");
    }
    if(strstr(Tmp, "gl_LightModelProducts") || strstr(Tmp, "gl_FrontLightModelProduct") || strstr(Tmp, "gl_BackLightModelProduct"))
    {
    Tmp = InplaceInsert(GetLine(Tmp, headline), gl4es_LightModelProductsSource, Tmp, &tmpsize);
    headline+=CountLine(gl4es_LightModelProductsSource);
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_LightModelProducts", "_gl4es_LightModelProducts");
    }
    if(strstr(Tmp, "gl_LightProducts") || strstr(Tmp, "gl_FrontLightProduct") || strstr(Tmp, "gl_BackLightProduct"))
    {
    Tmp = InplaceInsert(GetLine(Tmp, headline), gl4es_LightProductsSource, Tmp, &tmpsize);
    headline+=CountLine(gl4es_LightProductsSource);
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_LightProducts", "_gl4es_LightProducts");
    }
    if(strstr(Tmp, "gl_MaterialParameters ") || (strstr(Tmp, "gl_FrontMaterial")) || strstr(Tmp, "gl_BackMaterial"))
    {
    Tmp = InplaceInsert(GetLine(Tmp, headline), gl4es_MaterialParametersSource, Tmp, &tmpsize);
    headline+=CountLine(gl4es_MaterialParametersSource);
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_MaterialParameters", "_gl4es_MaterialParameters");
    }
    if(strstr(Tmp, "gl_LightSource")) {
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_LightSource", "_gl4es_LightSource");
    }
    if(strstr(Tmp, "gl_LightModel"))
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_LightModel", "_gl4es_LightModel");
    if(strstr(Tmp, "gl_FrontLightModelProduct"))
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_FrontLightModelProduct", "_gl4es_FrontLightModelProduct");
    if(strstr(Tmp, "gl_BackLightModelProduct"))
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_BackLightModelProduct", "_gl4es_BackLightModelProduct");
    if(strstr(Tmp, "gl_FrontLightProduct"))
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_FrontLightProduct", "_gl4es_FrontLightProduct");
    if(strstr(Tmp, "gl_BackLightProduct"))
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_BackLightProduct", "_gl4es_BackLightProduct");
    if(strstr(Tmp, "gl_FrontMaterial"))
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_FrontMaterial", "_gl4es_FrontMaterial");
    if(strstr(Tmp, "gl_BackMaterial"))
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_BackMaterial", "_gl4es_BackMaterial");
    if(strstr(Tmp, "gl_MaxLights"))
    {
    Tmp = InplaceInsert(GetLine(Tmp, 2), gl4es_MaxLightsSource, Tmp, &tmpsize);
    headline+=CountLine(gl4es_MaxLightsSource);
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_MaxLights", "_gl4es_MaxLights");
    }
    if(strstr(Tmp, "gl_NormalScale")) {
    Tmp = InplaceInsert(GetLine(Tmp, headline), gl4es_normalscaleSource, Tmp, &tmpsize);
    headline+=CountLine(gl4es_normalscaleSource);
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_NormalScale", "_gl4es_NormalScale");
    }
    if(strstr(Tmp, "gl_InstanceID") || strstr(Tmp, "gl_InstanceIDARB")) {
    Tmp = InplaceInsert(GetLine(Tmp, headline), gl4es_instanceID, Tmp, &tmpsize);
    headline+=CountLine(gl4es_instanceID);
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_InstanceIDARB", "_gl4es_InstanceID");
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_InstanceID", "_gl4es_InstanceID");
    }
    if(strstr(Tmp, "gl_ClipPlane")) {
    Tmp = InplaceInsert(GetLine(Tmp, headline), gl4es_clipplanesSource, Tmp, &tmpsize);
    headline+=CountLine(gl4es_clipplanesSource);
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_ClipPlane", "_gl4es_ClipPlane");
    }
    if(strstr(Tmp, "gl_MaxClipPlanes")) {
    Tmp = InplaceInsert(GetLine(Tmp, 2), gl4es_MaxClipPlanesSource, Tmp, &tmpsize);
    headline+=CountLine(gl4es_MaxClipPlanesSource);
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_MaxClipPlanes", "_gl4es_MaxClipPlanes");
    }
    
    if(strstr(Tmp, "gl_PointParameters") || strstr(Tmp, "gl_Point"))
    {
      Tmp = InplaceInsert(GetLine(Tmp, headline), gl4es_PointSpriteSource, Tmp, &tmpsize);
      headline+=CountLine(gl4es_PointSpriteSource);
      Tmp = InplaceReplace(Tmp, &tmpsize, "gl_PointParameters", "_gl4es_PointParameters");
    }
    if(strstr(Tmp, "gl_Point"))
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_Point", "_gl4es_Point");
    if(strstr(Tmp, "gl_FogParameters") || strstr(Tmp, "gl_Fog"))
    {
      Tmp = InplaceInsert(GetLine(Tmp, headline), hardext.highp?gl4es_FogParametersSourceHighp:gl4es_FogParametersSource, Tmp, &tmpsize);
      headline+=CountLine(gl4es_FogParametersSource);
      Tmp = InplaceReplace(Tmp, &tmpsize, "gl_FogParameters", "_gl4es_FogParameters");
    }
    if(strstr(Tmp, "gl_Fog"))
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_Fog", "_gl4es_Fog");
    if(strstr(Tmp, "gl_TextureEnvColor")) {
    Tmp = InplaceInsert(GetLine(Tmp, headline), gl4es_texenvcolorSource, Tmp, &tmpsize);
    headline+=CountLine(gl4es_texenvcolorSource);
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_TextureEnvColor", "_gl4es_TextureEnvColor");
    }
    if(strstr(Tmp, "gl_EyePlaneS")) {
    Tmp = InplaceInsert(GetLine(Tmp, headline), gl4es_texgeneyeSource[0], Tmp, &tmpsize);
    headline+=CountLine(gl4es_texgeneyeSource[0]);
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_EyePlaneS", "_gl4es_EyePlaneS");
    }
    if(strstr(Tmp, "gl_EyePlaneT")) {
    Tmp = InplaceInsert(GetLine(Tmp, headline), gl4es_texgeneyeSource[1], Tmp, &tmpsize);
    headline+=CountLine(gl4es_texgeneyeSource[1]);
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_EyePlaneT", "_gl4es_EyePlaneT");
    }
    if(strstr(Tmp, "gl_EyePlaneR")) {
    Tmp = InplaceInsert(GetLine(Tmp, headline), gl4es_texgeneyeSource[2], Tmp, &tmpsize);
    headline+=CountLine(gl4es_texgeneyeSource[2]);
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_EyePlaneR", "_gl4es_EyePlaneR");
    }
    if(strstr(Tmp, "gl_EyePlaneQ")) {
    Tmp = InplaceInsert(GetLine(Tmp, headline), gl4es_texgeneyeSource[3], Tmp, &tmpsize);
    headline+=CountLine(gl4es_texgeneyeSource[3]);
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_EyePlaneQ", "_gl4es_EyePlaneQ");
    }
    if(strstr(Tmp, "gl_ObjectPlaneS")) {
    Tmp = InplaceInsert(GetLine(Tmp, headline), gl4es_texgenobjSource[0], Tmp, &tmpsize);
    headline+=CountLine(gl4es_texgenobjSource[0]);
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_ObjectPlaneS", "_gl4es_ObjectPlaneS");
    }
    if(strstr(Tmp, "gl_ObjectPlaneT")) {
    Tmp = InplaceInsert(GetLine(Tmp, headline), gl4es_texgenobjSource[1], Tmp, &tmpsize);
    headline+=CountLine(gl4es_texgenobjSource[1]);
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_ObjectPlaneT", "_gl4es_ObjectPlaneT");
    }
    if(strstr(Tmp, "gl_ObjectPlaneR")) {
    Tmp = InplaceInsert(GetLine(Tmp, headline), gl4es_texgenobjSource[2], Tmp, &tmpsize);
    headline+=CountLine(gl4es_texgenobjSource[2]);
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_ObjectPlaneR", "_gl4es_ObjectPlaneR");
    }
    if(strstr(Tmp, "gl_ObjectPlaneQ")) {
    Tmp = InplaceInsert(GetLine(Tmp, headline), gl4es_texgenobjSource[3], Tmp, &tmpsize);
    headline+=CountLine(gl4es_texgenobjSource[3]);
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_ObjectPlaneQ", "_gl4es_ObjectPlaneQ");
    }
    
    if(strstr(Tmp, "gl_MaxTextureUnits")) {
    Tmp = InplaceInsert(GetLine(Tmp, 2), gl4es_MaxTextureUnitsSource, Tmp, &tmpsize);
    headline+=CountLine(gl4es_MaxTextureUnitsSource);
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_MaxTextureUnits", "_gl4es_MaxTextureUnits");
    }
    if(strstr(Tmp, "gl_MaxTextureCoords")) {
    Tmp = InplaceInsert(GetLine(Tmp, 2), gl4es_MaxTextureCoordsSource, Tmp, &tmpsize);
    headline+=CountLine(gl4es_MaxTextureCoordsSource);
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_MaxTextureCoords", "_gl4es_MaxTextureCoords");
    }
    if(strstr(Tmp, "gl_ClipVertex")) {
    // gl_ClipVertex is not handled for now
    // Proper way would be to copy handling from fpe_shader, but then, need to use gl_ClipPlane...
    static int ncv = 0;
    char CV[60];
    sprintf(CV, gl4es_dummyClipVertex, ncv);
    ++ncv;
    Tmp = InplaceReplace(Tmp, &tmpsize, "gl_ClipVertex", CV);
    }
    //oldprogram uniforms...
    if(FindString(Tmp, gl_ProgramEnv)) {
    // check if array can be removed
    int maxind = -1;
    int noarray_ok = 1;
    char* p = Tmp;
    while(noarray_ok && (p=FindStringNC(p, gl_ProgramEnv))) {
      p+=strlen(gl_ProgramEnv);
      if(*p=='[') {
        ++p;
        if(*p>='0' && *p<='9') {
          int n = (*p) - '0';
          if(p[1]>='0' && p[1]<='9')
            n = n*10 + (p[1] - '0');
          if (maxind<n) maxind = n;
        } else 
          noarray_ok=0;
      } else
        noarray_ok=0;
    }
    if(noarray_ok) {
      // ok, so change array to single...
      char F[60], T[60], U[300];
      for(int i=0; i<=maxind; ++i) {
        sprintf(F, "%s[%d]", gl_ProgramEnv, i);
        sprintf(T, "_gl4es_%s_ProgramEnv_%d", isVertex?"Vertex":"Fragment", i);
        Tmp = InplaceReplace(Tmp, &tmpsize, F, T);
        if(FindString(Tmp, T)) {
          // add the uniform declaration if needed
          sprintf(U, "uniform vec4 %s;\n", T);
          Tmp = InplaceInsert(GetLine(Tmp, headline), U, Tmp, &tmpsize);
          headline += 1;
        }
      }
    } else {
      // need the full array...
      char T[60], U[300];
      sprintf(T, "_gl4es_%s_ProgramEnv", isVertex?"Vertex":"Fragment");
      sprintf(U, "uniform vec4 %s[%d];\n", T, isVertex?MAX_VTX_PROG_ENV_PARAMS:MAX_FRG_PROG_ENV_PARAMS);
      Tmp = InplaceInsert(GetLine(Tmp, headline), U, Tmp, &tmpsize);
      headline += 1;
      Tmp = InplaceReplace(Tmp, &tmpsize, gl_ProgramEnv, T);
    }
    }
    if(FindString(Tmp, gl_ProgramLocal)) {
    // check if array can be removed
    int maxind = -1;
    int noarray_ok = 1;
    char* p = Tmp;
    while(noarray_ok && (p=FindStringNC(p, gl_ProgramLocal))) {
      p+=strlen(gl_ProgramLocal);
      if(*p=='[') {
        ++p;
        if(*p>='0' && *p<='9') {
          int n = (*p) - '0';
          if(p[1]>='0' && p[1]<='9')
            n = n*10 + (p[1] - '0');
          if (maxind<n) maxind = n;
        } else 
          noarray_ok=0;
      } else
        noarray_ok=0;
    }
    if(noarray_ok) {
      // ok, so change array to single...
      char F[60], T[60], U[300];
      for(int i=0; i<=maxind; ++i) {
        sprintf(F, "%s[%d]", gl_ProgramLocal, i);
        sprintf(T, "_gl4es_%s_ProgramLocal_%d", isVertex?"Vertex":"Fragment", i);
        Tmp = InplaceReplace(Tmp, &tmpsize, F, T);
        if(FindString(Tmp, T)) {
          // add the uniform declaration if needed
          sprintf(U, "uniform vec4 %s;\n", T);
          Tmp = InplaceInsert(GetLine(Tmp, headline), U, Tmp, &tmpsize);
          headline += 1;
        }
      }
    } else {
      // need the full array...
      char T[60], U[300];
      sprintf(T, "_gl4es_%s_ProgramLocal", isVertex?"Vertex":"Fragment");
      sprintf(U, "uniform vec4 %s[%d];\n", T, isVertex?MAX_VTX_PROG_LOC_PARAMS:MAX_FRG_PROG_LOC_PARAMS);
      Tmp = InplaceInsert(GetLine(Tmp, headline), U, Tmp, &tmpsize);
      headline += 1;
      Tmp = InplaceReplace(Tmp, &tmpsize, gl_ProgramLocal, T);
    }
    }
    #define GO(A) \
    if(strstr(Tmp, gl_Samplers ## A)) {                                   \
    char S[60], D[60], U[60];                                           \
    for(int i=0; i<MAX_TEX; ++i) {                                      \
      sprintf(S, "%s%d", gl_Samplers ## A, i);                          \
      if(FindString(Tmp, S)) {                                          \
        sprintf(D, "%s%d", gl4es_Samplers ## A, i);                     \
        sprintf(U, "%s%d;\n", gl4es_Samplers ## A ## _uniform, i);      \
        Tmp = InplaceReplace(Tmp, &tmpsize, S, D);                      \
        Tmp = InplaceInsert(GetLine(Tmp, headline), U, Tmp, &tmpsize);  \
        headline += 1;                                                  \
      }                                                                 \
    }                                                                   \
    }
    GO(1D)
    GO(2D)
    GO(3D)
    GO(Cube)
    #undef GO
    
    // non-square matrix handling
    // the square one first
    if(strstr(Tmp, "mat2x2")) {
    // better to use #define ?
    Tmp = InplaceReplace(Tmp, &tmpsize, "mat2x2", "mat2");
    }
    if(strstr(Tmp, "mat3x3")) {
    // better to use #define ?
    Tmp = InplaceReplace(Tmp, &tmpsize, "mat3x3", "mat3");
    }

/*
    if(strstr(Tmp, "#version 100")) {
    // better to use #define ?
    Tmp = InplaceReplace(Tmp, &tmpsize, "#version 100", "#version 150");
    }
   if(strstr(Tmp, "#version 110")) {
    // better to use #define ?
    Tmp = InplaceReplace(Tmp, &tmpsize, "#version 110", "#version 150");
    }
   if(strstr(Tmp, "#version 120")) {
    // better to use #define ?
    Tmp = InplaceReplace(Tmp, &tmpsize, "#version 120", "#version 150");
    }
   if(strstr(Tmp, "#version 130")) {
    // better to use #define ?
    Tmp = InplaceReplace(Tmp, &tmpsize, "#version 130", "#version 140");
    }
   if(strstr(Tmp, "#version 140")) {
    // better to use #define ?
    Tmp = InplaceReplace(Tmp, &tmpsize, "#version 140", "#version 150");
    }
*/

    // finish
    if((globals4es.dbgshaderconv&maskafter)==maskafter) {
    printf("New Shader source:\n%s\n", Tmp);
    }
    // clean preproc'd source
    if(pEntry!=pBuffer)
    free(pBuffer);
    return Tmp;
}

int isBuiltinAttrib(const char* name) {
    int n = sizeof(builtin_attrib)/sizeof(builtin_attrib_t);
    for (int i=0; i<n; i++) {
        if (strcmp(builtin_attrib[i].name, name)==0)
            return builtin_attrib[i].attrib;
    }
    return -1;
}

int isBuiltinMatrix(const char* name) {
    int ret = -1;
    int n = sizeof(builtin_matrix)/sizeof(builtin_matrix_t);
    for (int i=0; i<n && ret==-1; i++) {
        if (strncmp(builtin_matrix[i].name, name, strlen(builtin_matrix[i].name))==0) {
            int l=strlen(builtin_matrix[i].name);
            if(strlen(name)==l 
            || (strlen(name)==l+3 && name[l]=='[' && builtin_matrix[i].texarray)
            || (strlen(name)==l+4 && name[l]=='[' && builtin_matrix[i].texarray)
            ) {
                ret=builtin_matrix[i].matrix;
                if(builtin_matrix[i].texarray) {
                    int n = name[l+1] - '0';
                    if(name[l+2]>='0' && name[l+2]<='9')
                      n = n*10 + name[l+2]-'0';
                    ret+=n*4;
                }
            }
        }
    }
    return ret;
}

const char* hasBuiltinAttrib(const char* vertexShader, int Att) {
    if(!vertexShader) // can happens (like if the shader is a pure GLES2 one)
      return NULL;
    // first search for the string
    const char* ret = NULL;
    if(hardext.maxvattrib>8) {
      int n = sizeof(builtin_attrib)/sizeof(builtin_attrib_t);
      for (int i=0; i<n && !ret; i++) {
          if (builtin_attrib[i].attrib == Att)
              ret = builtin_attrib[i].name;
      }
    } else {
      int n = sizeof(builtin_attrib_compressed)/sizeof(builtin_attrib_t);
      for (int i=0; i<n && !ret; i++) {
          if (builtin_attrib_compressed[i].attrib == Att)
              ret = builtin_attrib_compressed[i].name;
      }
    }
    if (!ret)
      return NULL;
    if(strstr(vertexShader, ret)) // it's here!
      return ret;
    // check for old program generic vertex attribs
    if(strstr(vertexShader, gl4es_VA[Att]))
      return gl4es_VA[Att];
    // nope
    return NULL;
}

const char* builtinAttribGLName(const char* name) {
  // no need to check for compressed array here...
  int n = sizeof(builtin_attrib)/sizeof(builtin_attrib_t);
  for(int i=0; i<n; ++i)
    if(!strcmp(name, builtin_attrib[i].name))
      return builtin_attrib[i].glname;
  if(!strncmp(name, gl4es_VertexAttrib, strlen(gl4es_VertexAttrib))) {
    int l = strlen(gl4es_VertexAttrib);
    int n = 0;
    while(name[l]>='0' && name[l]<='9')
      n = n*10 + name[l++]-'0';
    return gl_VA[n];
  }
  return name;
}

const char* builtinAttribInternalName(const char* name) {
  // no need to check for compressed array here...
  int n = sizeof(builtin_attrib)/sizeof(builtin_attrib_t);
  for(int i=0; i<n; ++i)
    if(!strcmp(name, builtin_attrib[i].glname))
      return builtin_attrib[i].name;
  if(!strncmp(name, gl_VertexAttrib, strlen(gl_VertexAttrib))) {
    int l = strlen(gl_VertexAttrib);
    int n = 0;
    while(name[l]>='0' && name[l]<='9')
      n = n*10 + name[l++]-'0';
    return gl4es_VA[n];
  }
  return name;
}
